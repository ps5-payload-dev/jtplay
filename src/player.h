// SPDX-License-Identifier: GPL-3.0-or-later
//
// FFmpeg playback pipeline, adapted from ps5-payload-dev/tvhp. The HTSP
// sources (live elementary streams, the DVR file API) are gone; the single
// source here is a URL demuxed by libavformat, which covers everything a
// DLNA server serves: http-get resources, with range-request seeking when
// the server supports it.
#ifndef PLAYER_H
#define PLAYER_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <SDL.h>

#include "visualizer.h"

// One selectable elementary stream of the open file.
struct TrackInfo {
  int stream_index = -1; // ffmpeg stream index, stable while the file is open
  std::string label;     // display string ("eng · ac3 5.1(side)", ...)
};

using AudioTrackInfo = TrackInfo; // "eng · ac3 5.1(side)"
using VideoTrackInfo = TrackInfo; // "h264 1920x1080"

// Audio format handed to SDL.
inline constexpr int kPlayerAudioRate = 48000;
inline constexpr int kPlayerAudioChannels = 2;
inline constexpr int kPlayerAudioBytesPerSec = kPlayerAudioRate * kPlayerAudioChannels * 2; // S16

// Converts decoded audio frames to the format the SDL device expects
// (S16, stereo, 48 kHz).
//
// The resampler is rebuilt whenever the input format changes; a seek can
// land in a differently configured part of the file, and handing
// swr_convert() a frame with fewer planes than the context was configured
// for makes it read past the end of the frame's plane array. The format is
// therefore re-checked on every frame.
class AudioResampler {
public:
  ~AudioResampler();

  // Converts one frame. Returns the number of samples per channel written to
  // 'out', 0 if the frame produced nothing, or -1 if it could not be
  // converted (in which case the frame should be skipped, not retried).
  int Convert(const AVFrame* frame, std::vector<uint8_t>& out);

  // Drops the resampler and anything it has buffered. Used after a seek, so
  // samples from before the jump are not emitted afterwards.
  void Reset();

private:
  bool Configure(const AVFrame* frame);

  SwrContext* swr_ = nullptr;
  AVSampleFormat in_fmt_ = AV_SAMPLE_FMT_NONE;
  int in_rate_ = 0;
  AVChannelLayout in_layout_ = {};
};

// Plays a URL by decoding it directly with libavcodec.
//
// Threads:
//   demux thread -> read + queue packets
//   video thread -> decode -> frame queue    (yuv420p, converted if needed)
//   audio thread -> decode -> swresample -> SDL audio queue (S16 stereo 48k)
//   main thread  -> RenderVideo(): pick the frame that is due, upload it to a
//                   streaming IYUV SDL_Texture and copy it letterboxed. The
//                   YUV->RGB conversion is done by SDL; no OpenGL is used.
//
// Open() and Stop() block on network and thread teardown respectively; the
// app calls them from its worker thread. RenderVideo() and the small
// queries below are safe from the main thread while the pipeline runs.
class Player {
public:
  Player();
  ~Player();

  // Main thread. Grabs the backend's SDL renderer, which must outlive this
  // object (or Shutdown() must be called before it is destroyed).
  bool Initialize(std::string& error);
  void Shutdown();

  // Opens 'url' and starts playback, replacing whatever is playing.
  // Blocking (network); call from a worker thread.
  bool Open(const std::string& url, std::string& error);

  // Stops playback and releases the demuxer. Blocking (joins threads).
  void Stop();

  bool IsPlaying() const { return active_; }

  // True once the file has been read to the end AND everything decoded has
  // been presented/played. The app uses it to leave the player or advance
  // to the next track.
  bool AtEnd() const;

  // Called every frame after the backbuffer has been cleared and before the
  // UI is rendered; the document body is transparent while playing, so the
  // UI composites on top. For audio-only playback there is no video plane,
  // so the spectrum visualizer is drawn in its place.
  void RenderVideo(int width, int height);

  // Short human-readable status ("h264 1920x1080 + aac", "Opening...",
  // ...) for display in the UI. Empty = nothing.
  std::string StatusText() const;

  // --- Transport ----------------------------------------------------------
  bool CanSeek() const { return seekable_; }
  // Position and duration in microseconds; -1 when unknown.
  int64_t PositionUs() const;
  int64_t DurationUs() const { return duration_us_.load(); }
  // Seeks by a relative amount; clamped to the file.
  void SeekRelative(int64_t delta_us);
  void SetPaused(bool paused);
  // Returns the new paused state.
  bool TogglePause();
  bool IsPaused() const { return paused_; }

  // --- Video tracks -------------------------------------------------------
  // The video streams found in the open file, in file order (attached
  // cover art excluded). Fixed for the lifetime of the file.
  std::vector<VideoTrackInfo> VideoTracks() const;
  int VideoTrackCount() const { return vtrack_count_.load(); }
  // ffmpeg stream index of the track being decoded, or -1.
  int CurrentVideoStream() const { return video_stream_.load(); }
  // Label of the track being decoded; "" when there is no video.
  std::string CurrentVideoLabel() const;
  // Switches decoding to another video stream of the open file. Same
  // contract as SelectAudioTrack().
  bool SelectVideoTrack(int stream_index);

  // --- Audio tracks -------------------------------------------------------
  // The audio streams found in the open file, in file order. Fixed for the
  // lifetime of the file; empty when nothing is open.
  std::vector<AudioTrackInfo> AudioTracks() const;
  int AudioTrackCount() const { return track_count_.load(); }
  // ffmpeg stream index of the track being decoded, or -1.
  int CurrentAudioStream() const { return audio_stream_.load(); }
  // Label of the track being decoded; "" when there is no audio.
  std::string CurrentAudioLabel() const;
  // Switches decoding to another audio stream of the open file. Returns
  // false if the index is unknown, already active, or no audio pipeline is
  // running. Non-blocking: the demux thread applies the switch and, when
  // the stream is seekable, resyncs to the current position.
  bool SelectAudioTrack(int stream_index);

private:
  // Demuxed packets waiting for a decoder. These are the demuxer's own
  // reference-counted buffers, so nothing is copied on the way in or out;
  // timestamps are rewritten to file-relative microseconds on ingest.
  struct PacketQueue {
    static constexpr size_t kMaxPackets = 256;

    mutable std::mutex mutex;
    std::condition_variable cv;
    std::deque<AVPacket*> q; // owned
    // Bumped on flush; the decode thread resets its codec state when it sees
    // the counter change, so a seek doesn't smear old frames into new ones.
    uint32_t flush_epoch = 0;

    ~PacketQueue() { Clear(); }

    // Takes ownership of 'pkt'. Drops it when the queue is full; the demux
    // thread checks Full() before reading.
    void Push(AVPacket* pkt);
    // Moves the next packet into 'out'. False once the pipeline stops.
    bool Pop(AVPacket* out, std::atomic<bool>& running, uint32_t* epoch);
    // Current flush counter, for a decode thread to notice that a seek
    // flushed the queue while it was blocked mid-frame.
    uint32_t Epoch() const;
    void Clear();
    bool Full() const;
    bool Empty() const;
  };

  struct TimedFrame {
    AVFrame* frame = nullptr;
    int64_t pts = INT64_MIN; // us
    // vqueue_ flush counter this frame was decoded under. A frame whose
    // epoch is stale comes from before a seek, so its pts belongs to a part
    // of the file that is no longer being played.
    uint32_t epoch = 0;
  };

  // Decode / present pipeline.
  bool OpenInput(const std::string& url, std::string& error);
  void CloseInput();
  bool StartPipeline();
  void StopPipeline();
  void VideoThread();
  void AudioThread();
  void DemuxThread();
  int64_t MasterClock() const; // us, or INT64_MIN if not started
  bool OpenAudioDevice();
  // Audio thread only: replaces actx_ with a decoder for another stream.
  bool ReopenAudioDecoder(int stream_index);
  // Video thread only: replaces vctx_ with a decoder for another stream.
  bool ReopenVideoDecoder(int stream_index);
  // Builds video_tracks_/audio_tracks_ from the open container.
  void CollectTracks();
  // 'epoch' is the vqueue_ flush counter the packet this frame came from was
  // popped under; the frame is dropped instead of queued if a seek has
  // flushed the queues since.
  void PushVideoFrame(AVFrame* frame, uint32_t epoch);
  void DropDecodedFrames();
  void SetStatus(const std::string& text);

  std::atomic<bool> active_{false};
  std::atomic<bool> threads_running_{false};

  // --- video ---
  // Written by OpenInput/CloseInput and by the demux thread on a track
  // switch; read from the decode threads and the main thread, hence atomic.
  std::atomic<int> video_stream_{-1};
  AVCodecContext* vctx_ = nullptr;
  SwsContext* sws_ = nullptr; // lazy, only if decoder output isn't yuv420p
  std::thread vthread_;
  PacketQueue vqueue_;

  // Selectable video tracks; fixed after OpenInput, cleared in CloseInput.
  // Guarded by tracks_mutex_, like the audio ones.
  std::vector<VideoTrackInfo> video_tracks_;
  std::atomic<int> vtrack_count_{0};
  // ffmpeg stream index to switch to; -1 = none pending. Set by
  // SelectVideoTrack(), consumed by the demux thread.
  std::atomic<int> video_switch_target_{-1};

  mutable std::mutex frames_mutex_;
  std::condition_variable frames_cv_;
  std::deque<TimedFrame> frames_; // decoded, in presentation order
  static constexpr size_t kMaxFrames = 4;

  // --- audio ---
  // Written by OpenInput/CloseInput and by the demux thread on a track
  // switch; read from the decode threads and the main thread, hence atomic.
  std::atomic<int> audio_stream_{-1};
  AVCodecContext* actx_ = nullptr;

  // Selectable audio tracks; fixed after OpenInput, cleared in CloseInput.
  mutable std::mutex tracks_mutex_;
  std::vector<AudioTrackInfo> audio_tracks_;
  std::atomic<int> track_count_{0};
  // ffmpeg stream index to switch to; -1 = none pending. Set by
  // SelectAudioTrack(), consumed by the demux thread.
  std::atomic<int> audio_switch_target_{-1};
  AudioResampler resampler_;
  // Fed by the audio thread with the same samples that go to the device;
  // drawn by RenderVideo() when the file has no video stream.
  Visualizer visualizer_;
  std::thread athread_;
  PacketQueue aqueue_;
  SDL_AudioDeviceID audio_dev_ = 0;
  static constexpr int kAudioRate = kPlayerAudioRate;
  static constexpr int kAudioChannels = kPlayerAudioChannels;
  static constexpr int kAudioBytesPerSec = kPlayerAudioBytesPerSec;

  // --- clock (us) ---
  std::atomic<int64_t> audio_pts_end_{INT64_MIN}; // pts at the end of queued audio
  std::atomic<int64_t> wall_anchor_pts_{INT64_MIN};
  std::atomic<int64_t> wall_anchor_time_{0};
  std::atomic<bool> paused_{false};

  // --- demuxing ---
  std::thread dthread_;
  AVFormatContext* fmt_ = nullptr;
  std::atomic<int64_t> duration_us_{-1};
  // Files often start at a non-zero container timestamp; everything
  // downstream works in file-relative microseconds, so this is subtracted on
  // ingest and added back when seeking.
  int64_t file_start_us_ = 0;
  std::atomic<int64_t> seek_target_us_{-1}; // >= 0 = seek pending
  std::atomic<int64_t> last_position_us_{0};
  std::atomic<bool> seekable_{false};
  std::atomic<bool> eof_{false};

  // --- presentation (SDL) ---
  SDL_Renderer* renderer_ = nullptr;
  SDL_Texture* texture_ = nullptr; // IYUV streaming texture
  int tex_w_ = 0, tex_h_ = 0;
  TimedFrame current_; // frame currently on screen (owned)

  mutable std::mutex status_mutex_;
  std::string status_;
};

#endif

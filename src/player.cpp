// SPDX-License-Identifier: GPL-3.0-or-later
//
// Adapted from ps5-payload-dev/tvhp's player.cpp (GPL-3.0). See player.h
// for what changed.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
}

#include "RmlUi/Backend.h"
#include "player.h"

// The visualizer is fed straight from the resampler's output, so the two
// must agree on the PCM format.
static_assert(kVizRate == kPlayerAudioRate, "visualizer sample rate mismatch");
static_assert(kVizChannels == kPlayerAudioChannels, "visualizer channel count mismatch");

namespace {

// Packets buffered ahead of each decoder.
constexpr size_t kPacketQueueDepth = 256;

} // namespace

// ---------------------------------------------------------------------------
// Packet queues
// ---------------------------------------------------------------------------

void Player::PacketQueue::Push(DemuxPacket&& pkt)
{
	std::lock_guard<std::mutex> lock(mutex);
	if (q.size() >= max_packets)
		return; // caller (the demux thread) retries after checking Full()
	q.push_back(std::move(pkt));
	cv.notify_one();
}

bool Player::PacketQueue::Pop(DemuxPacket& out, std::atomic<bool>& running, uint32_t* epoch)
{
	std::unique_lock<std::mutex> lock(mutex);
	cv.wait(lock, [&] { return !q.empty() || !running; });
	if (!running)
		return false;
	if (epoch)
		*epoch = flush_epoch;
	out = std::move(q.front());
	q.pop_front();
	return true;
}

uint32_t Player::PacketQueue::Epoch()
{
	std::lock_guard<std::mutex> lock(mutex);
	return flush_epoch;
}

void Player::PacketQueue::Clear()
{
	std::lock_guard<std::mutex> lock(mutex);
	q.clear();
	flush_epoch++;
	cv.notify_all();
}

bool Player::PacketQueue::Full()
{
	std::lock_guard<std::mutex> lock(mutex);
	return q.size() >= max_packets;
}

// ---------------------------------------------------------------------------
// Audio resampling
// ---------------------------------------------------------------------------

AudioResampler::~AudioResampler()
{
	Reset();
}

void AudioResampler::Reset()
{
	if (swr_)
		swr_free(&swr_);
	in_fmt_ = AV_SAMPLE_FMT_NONE;
	in_rate_ = 0;
	av_channel_layout_uninit(&in_layout_);
}

bool AudioResampler::Configure(const AVFrame* frame)
{
	Reset();

	AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
	if (swr_alloc_set_opts2(&swr_, &out_layout, AV_SAMPLE_FMT_S16, kPlayerAudioRate,
		&frame->ch_layout, (AVSampleFormat)frame->format, frame->sample_rate, 0, nullptr) < 0)
	{
		// swr_alloc_set_opts2 may still have allocated the context.
		Reset();
		return false;
	}
	if (!swr_ || swr_init(swr_) < 0)
	{
		// A context that failed to initialise must not be kept: swr_convert()
		// on it returns an error at best, and the next frame would skip the
		// configure step entirely because the pointer is non-null.
		Reset();
		return false;
	}

	in_fmt_ = (AVSampleFormat)frame->format;
	in_rate_ = frame->sample_rate;
	if (av_channel_layout_copy(&in_layout_, &frame->ch_layout) < 0)
	{
		Reset();
		return false;
	}
	return true;
}

int AudioResampler::Convert(const AVFrame* frame, std::vector<uint8_t>& out)
{
	if (!frame || frame->nb_samples <= 0 || frame->sample_rate <= 0 ||
	    frame->ch_layout.nb_channels <= 0 || frame->format < 0)
		return -1;

	// Rebuild whenever the input format differs from what the context was
	// configured for. Comparing the layout matters most: a mismatch there is
	// what reads past the end of frame->extended_data.
	const bool stale = !swr_ ||
		in_fmt_ != (AVSampleFormat)frame->format ||
		in_rate_ != frame->sample_rate ||
		av_channel_layout_compare(&in_layout_, &frame->ch_layout) != 0;
	if (stale && !Configure(frame))
		return -1;

	const int max_out = swr_get_out_samples(swr_, frame->nb_samples);
	if (max_out <= 0)
		return max_out == 0 ? 0 : -1;

	out.resize((size_t)max_out * kPlayerAudioChannels * 2);
	uint8_t* planes[1] = {out.data()};
	const int got = swr_convert(swr_, planes, max_out,
		(const uint8_t**)frame->extended_data, frame->nb_samples);
	if (got < 0)
		return -1;
	return got;
}

// ---------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------

Player::Player() = default;

Player::~Player()
{
	StopPipeline();
	CloseInput();
}

bool Player::Initialize(std::string& error)
{
	renderer_ = Backend::GetSDLRenderer();
	if (!renderer_)
	{
		error = "no SDL renderer";
		return false;
	}
	avformat_network_init();
	return true; // the texture is created lazily, once the frame size is known
}

void Player::Shutdown()
{
	Stop();

	if (current_.frame)
	{
		av_frame_free(&current_.frame);
		current_ = {};
	}
	if (texture_)
	{
		SDL_DestroyTexture(texture_);
		texture_ = nullptr;
	}
	tex_w_ = tex_h_ = 0;
	// Owns an SDL texture too, and renderer_ is about to be dropped.
	visualizer_.Shutdown();
	renderer_ = nullptr;
	avformat_network_deinit();
}

std::string Player::StatusText() const
{
	std::lock_guard<std::mutex> lock(status_mutex_);
	return status_;
}

void Player::SetStatus(const std::string& text)
{
	std::lock_guard<std::mutex> lock(status_mutex_);
	status_ = text;
}

// ---------------------------------------------------------------------------
// Input (libavformat over HTTP)
// ---------------------------------------------------------------------------

// Enumerates the selectable video and audio streams of the open container.
// The label is built from the stream metadata (language, title) plus the
// codec and, depending on the kind, the channel layout or the frame size:
// "eng · ac3 5.1(side)", "Commentary · aac stereo", "h264 1920x1080".
void Player::CollectTracks()
{
	std::lock_guard<std::mutex> lock(tracks_mutex_);
	video_tracks_.clear();
	audio_tracks_.clear();

	for (unsigned i = 0; fmt_ && i < fmt_->nb_streams; i++)
	{
		const AVStream* st = fmt_->streams[i];
		const AVMediaType type = st->codecpar->codec_type;
		if (type != AVMEDIA_TYPE_VIDEO && type != AVMEDIA_TYPE_AUDIO)
			continue;
		// Cover art is a video stream in name only; not selectable.
		if (type == AVMEDIA_TYPE_VIDEO && (st->disposition & AV_DISPOSITION_ATTACHED_PIC))
			continue;

		std::string label;
		auto append = [&label](const std::string& part) {
			if (part.empty())
				return;
			label += (label.empty() ? "" : " · ") + part;
		};
		const AVDictionaryEntry* lang = av_dict_get(st->metadata, "language", nullptr, 0);
		if (lang && lang->value[0] && std::strcmp(lang->value, "und") != 0)
			append(lang->value);
		const AVDictionaryEntry* title = av_dict_get(st->metadata, "title", nullptr, 0);
		if (title && title->value[0])
			append(title->value);

		std::string codec = avcodec_get_name(st->codecpar->codec_id);
		if (type == AVMEDIA_TYPE_AUDIO)
		{
			char layout[64];
			if (av_channel_layout_describe(&st->codecpar->ch_layout, layout, sizeof(layout)) > 0)
				codec += std::string(" ") + layout;
		}
		else if (st->codecpar->width > 0 && st->codecpar->height > 0)
		{
			codec += " " + std::to_string(st->codecpar->width) + "x" +
				std::to_string(st->codecpar->height);
		}
		append(codec);

		std::vector<TrackInfo>& into =
			(type == AVMEDIA_TYPE_AUDIO) ? audio_tracks_ : video_tracks_;
		TrackInfo t;
		t.stream_index = (int)i;
		t.label = label.empty() ? "Track " + std::to_string(into.size() + 1) : label;
		into.push_back(std::move(t));
	}

	vtrack_count_ = (int)video_tracks_.size();
	track_count_ = (int)audio_tracks_.size();
}

bool Player::OpenInput(const std::string& url, std::string& error)
{
	CloseInput();

	fmt_ = avformat_alloc_context();
	if (!fmt_)
	{
		error = "out of memory";
		return false;
	}

	AVDictionary* opts = nullptr;
	// A stalled server should error out instead of hanging a thread forever.
	av_dict_set(&opts, "rw_timeout", "15000000", 0); // us
	// Reconnect on dropped connections mid-stream; DLNA servers recycle
	// sockets aggressively.
	av_dict_set(&opts, "reconnect", "1", 0);
	av_dict_set(&opts, "reconnect_streamed", "1", 0);
	av_dict_set(&opts, "reconnect_delay_max", "5", 0);

	const int rc = avformat_open_input(&fmt_, url.c_str(), nullptr, &opts);
	av_dict_free(&opts);
	if (rc < 0)
	{
		// avformat_open_input frees fmt_ on failure.
		fmt_ = nullptr;
		char buf[128];
		av_strerror(rc, buf, sizeof(buf));
		error = std::string("unable to open the stream: ") + buf;
		return false;
	}
	if (avformat_find_stream_info(fmt_, nullptr) < 0)
	{
		error = "unable to identify the streams";
		CloseInput();
		return false;
	}

	video_stream_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	audio_stream_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
	if (video_stream_ < 0 && audio_stream_ < 0)
	{
		error = "no playable streams";
		CloseInput();
		return false;
	}

	CollectTracks();

	// Cover art in audio files shows up as an attached-picture video stream;
	// decoding it as a movie makes no sense, so drop it here. A file can
	// carry both, so fall back to a real video track before giving up on
	// video entirely. The UI shows the DIDL album art instead.
	if (video_stream_ >= 0 &&
	    (fmt_->streams[video_stream_]->disposition & AV_DISPOSITION_ATTACHED_PIC))
	{
		std::lock_guard<std::mutex> lock(tracks_mutex_);
		video_stream_ = video_tracks_.empty() ? -1 : video_tracks_.front().stream_index;
	}

	// Decoders are opened straight from the container's stream parameters.
	std::string desc;
	auto open_stream = [&](int index, AVCodecContext** out) -> bool {
		if (index < 0)
			return false;
		AVStream* st = fmt_->streams[index];
		const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
		if (!codec)
			return false;
		AVCodecContext* ctx = avcodec_alloc_context3(codec);
		if (!ctx)
			return false;
		ctx->thread_count = 0;
		if (avcodec_parameters_to_context(ctx, st->codecpar) < 0 || avcodec_open2(ctx, codec, nullptr) < 0)
		{
			avcodec_free_context(&ctx);
			return false;
		}
		ctx->pkt_timebase = st->time_base;
		*out = ctx;
		return true;
	};

	if (open_stream(video_stream_, &vctx_))
	{
		desc = avcodec_get_name(vctx_->codec_id);
		if (vctx_->width && vctx_->height)
			desc += " " + std::to_string(vctx_->width) + "x" + std::to_string(vctx_->height);
	}
	else
	{
		video_stream_ = -1;
	}
	if (open_stream(audio_stream_, &actx_))
	{
		desc += (desc.empty() ? "" : " + ") + std::string(avcodec_get_name(actx_->codec_id));
	}
	else
	{
		audio_stream_ = -1;
	}

	if (video_stream_ < 0 && audio_stream_ < 0)
	{
		error = "no decoder available for this stream";
		CloseInput();
		return false;
	}

	file_start_us_ = (fmt_->start_time != AV_NOPTS_VALUE) ? fmt_->start_time : 0;
	duration_us_ = (fmt_->duration != AV_NOPTS_VALUE && fmt_->duration > 0) ? fmt_->duration : -1;
	seekable_ = fmt_->pb && (fmt_->pb->seekable & AVIO_SEEKABLE_NORMAL) && duration_us_ > 0;
	SetStatus(desc);
	return true;
}

void Player::CloseInput()
{
	if (fmt_)
		avformat_close_input(&fmt_); // also frees fmt_
	{
		std::lock_guard<std::mutex> lock(tracks_mutex_);
		video_tracks_.clear();
		audio_tracks_.clear();
	}
	vtrack_count_ = 0;
	track_count_ = 0;
	video_switch_target_ = -1;
	audio_switch_target_ = -1;
	video_stream_ = audio_stream_ = -1;
	file_start_us_ = 0;
	duration_us_ = -1;
	seek_target_us_ = -1;
	last_position_us_ = 0;
	seekable_ = false;
	eof_ = false;

	if (current_.frame)
	  av_frame_free(&current_.frame);
	current_.frame = nullptr;
	
}

bool Player::Open(const std::string& url, std::string& error)
{
	Stop();

	SetStatus("Opening...");
	if (!OpenInput(url, error))
	{
		SetStatus({});
		return false;
	}

	vqueue_.max_packets = aqueue_.max_packets = kPacketQueueDepth;

	if (!StartPipeline())
	{
		CloseInput();
		SetStatus({});
		error = "failed to start the decoders";
		return false;
	}

	dthread_ = std::thread(&Player::DemuxThread, this);
	active_ = true;
	return true;
}

void Player::Stop()
{
	active_ = false;
	StopPipeline();
	CloseInput();
	SetStatus({});
}

void Player::DemuxThread()
{
	AVPacket* pkt = av_packet_alloc();

	while (threads_running_)
	{
		// Pending video track switch: same shape as the audio one below.
		// The frame queue is dropped as well, otherwise frames decoded
		// from the old stream would be presented after the switch, and
		// the wall clock is unanchored because the new track's first
		// frame re-establishes it when there is no audio to follow.
		const int vswitch_to = video_switch_target_.exchange(-1);
		if (vswitch_to >= 0 && vswitch_to != video_stream_.load())
		{
			const int64_t pos = PositionUs();
			video_stream_ = vswitch_to;
			vqueue_.Clear(); // epoch bump: the video thread drops its tail
			DropDecodedFrames();
			if (audio_stream_.load() < 0)
				wall_anchor_pts_ = INT64_MIN;
			if (pos >= 0)
			{
				last_position_us_ = pos;
				if (seekable_)
					seek_target_us_ = pos;
			}
			continue;
		}

		// Pending audio track switch: retarget which stream is queued and
		// flush the audio side; the decode thread rebuilds its decoder when
		// it sees packets from the new stream. When the file is seekable,
		// jump back to the current position so the new track picks up where
		// the old one was *heard*, not where the demuxer had read ahead to.
		const int switch_to = audio_switch_target_.exchange(-1);
		if (switch_to >= 0 && switch_to != audio_stream_.load())
		{
			const int64_t pos = PositionUs();
			audio_stream_ = switch_to;
			aqueue_.Clear(); // epoch bump: the audio thread drops its tail
			if (audio_dev_)
				SDL_ClearQueuedAudio(audio_dev_);
			audio_pts_end_ = INT64_MIN;
			if (pos >= 0)
			{
				last_position_us_ = pos;
				if (seekable_)
					seek_target_us_ = pos;
			}
			continue;
		}

		// Pending seek: reposition, then flush the queues so the decoders
		// throw away everything from before the jump.
		const int64_t seek_us = seek_target_us_.exchange(-1);
		if (seek_us >= 0)
		{
			if (avformat_seek_file(fmt_, -1, INT64_MIN, seek_us + file_start_us_, INT64_MAX, 0) >= 0)
			{
				vqueue_.Clear();
				aqueue_.Clear();
				DropDecodedFrames();
				if (audio_dev_)
					SDL_ClearQueuedAudio(audio_dev_);
				audio_pts_end_ = INT64_MIN;
				wall_anchor_pts_ = INT64_MIN;
				last_position_us_ = seek_us;
				eof_ = false;
				if (fmt_->pb)
					fmt_->pb->eof_reached = 0;
			}
			continue;
		}

		if (paused_ || eof_ || vqueue_.Full() || aqueue_.Full())
		{
			// Nothing to do right now; sleep briefly so a seek or a resume
			// is picked up quickly.
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			continue;
		}

		const int rc = av_read_frame(fmt_, pkt);
		if (rc < 0)
		{
			if (rc == AVERROR_EOF)
				eof_ = true; // stays set until a seek clears it
			else
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
			continue;
		}

		if (pkt->stream_index == video_stream_ || pkt->stream_index == audio_stream_)
		{
			const AVRational tb = fmt_->streams[pkt->stream_index]->time_base;
			DemuxPacket out;
			out.stream = (uint32_t)pkt->stream_index;
			out.pts = (pkt->pts != AV_NOPTS_VALUE)
				? av_rescale_q(pkt->pts, tb, AVRational{1, 1000000}) - file_start_us_ : INT64_MIN;
			out.dts = (pkt->dts != AV_NOPTS_VALUE)
				? av_rescale_q(pkt->dts, tb, AVRational{1, 1000000}) - file_start_us_ : INT64_MIN;
			out.frametype = (pkt->flags & AV_PKT_FLAG_KEY) ? 'I' : 0;
			out.payload.assign(pkt->data, pkt->data + pkt->size);
			if (pkt->stream_index == video_stream_)
				vqueue_.Push(std::move(out));
			else
				aqueue_.Push(std::move(out));
		}
		av_packet_unref(pkt);
	}

	av_packet_free(&pkt);
}

// ---------------------------------------------------------------------------
// Transport
// ---------------------------------------------------------------------------

int64_t Player::PositionUs() const
{
	if (!active_)
		return -1;
	// Video playback tracks the presented frame (RenderVideo). Audio-only
	// playback has no presented frame, so the master clock is the position.
	if (video_stream_ < 0)
	{
		const int64_t clock = MasterClock();
		if (clock != INT64_MIN)
			return std::max<int64_t>(0, clock);
	}
	return last_position_us_.load();
}

void Player::SeekRelative(int64_t delta_us)
{
	if (!CanSeek())
		return;

	const int64_t duration = duration_us_.load();
	int64_t target = PositionUs() + delta_us;
	if (target < 0)
		target = 0;
	if (duration > 0 && target > duration - 1000000)
		target = std::max<int64_t>(0, duration - 1000000);

	last_position_us_ = target; // so repeated presses accumulate
	seek_target_us_ = target;
}

void Player::SetPaused(bool paused)
{
	if (paused_ == paused)
		return;

	paused_ = paused;
	if (audio_dev_)
		SDL_PauseAudioDevice(audio_dev_, paused ? 1 : 0);

	if (!paused)
	{
		// Re-anchor the fallback wall clock so it doesn't jump by the length
		// of the pause on resume.
		if (wall_anchor_pts_ != INT64_MIN)
			wall_anchor_time_ = av_gettime_relative();
	}
}

bool Player::TogglePause()
{
	SetPaused(!paused_);
	return paused_;
}

std::vector<VideoTrackInfo> Player::VideoTracks() const
{
	std::lock_guard<std::mutex> lock(tracks_mutex_);
	return video_tracks_;
}

std::vector<AudioTrackInfo> Player::AudioTracks() const
{
	std::lock_guard<std::mutex> lock(tracks_mutex_);
	return audio_tracks_;
}

// Label lookups: the same walk the UI would do, but under the one lock, so
// the caller cannot see a track list from a file that has since closed.
std::string Player::CurrentVideoLabel() const
{
	const int cur = video_stream_.load();
	std::lock_guard<std::mutex> lock(tracks_mutex_);
	for (const VideoTrackInfo& t : video_tracks_)
		if (t.stream_index == cur)
			return t.label;
	return {};
}

std::string Player::CurrentAudioLabel() const
{
	const int cur = audio_stream_.load();
	std::lock_guard<std::mutex> lock(tracks_mutex_);
	for (const AudioTrackInfo& t : audio_tracks_)
		if (t.stream_index == cur)
			return t.label;
	return {};
}

bool Player::SelectVideoTrack(int stream_index)
{
	// Only meaningful while a video pipeline is running; an audio-only
	// file (or one whose video decoder failed to open) has no thread to
	// feed the packets to.
	if (!active_ || video_stream_.load() < 0)
		return false;
	if (stream_index == video_stream_.load())
		return false;
	{
		std::lock_guard<std::mutex> lock(tracks_mutex_);
		bool known = false;
		for (const VideoTrackInfo& t : video_tracks_)
			known = known || t.stream_index == stream_index;
		if (!known)
			return false;
	}
	video_switch_target_ = stream_index;
	return true;
}

bool Player::SelectAudioTrack(int stream_index)
{
	// Only meaningful while an audio pipeline is running; if the initial
	// audio decoder could not be opened there is no thread to feed.
	if (!active_ || audio_stream_.load() < 0)
		return false;
	if (stream_index == audio_stream_.load())
		return false;
	{
		std::lock_guard<std::mutex> lock(tracks_mutex_);
		bool known = false;
		for (const AudioTrackInfo& t : audio_tracks_)
			known = known || t.stream_index == stream_index;
		if (!known)
			return false;
	}
	audio_switch_target_ = stream_index;
	return true;
}

bool Player::AtEnd() const
{
	if (!active_ || !eof_)
		return false;
	{
		std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(frames_mutex_));
		if (!frames_.empty())
			return false;
	}
	auto& vq = const_cast<PacketQueue&>(vqueue_);
	auto& aq = const_cast<PacketQueue&>(aqueue_);
	{
		std::lock_guard<std::mutex> lock(vq.mutex);
		if (!vq.q.empty())
			return false;
	}
	{
		std::lock_guard<std::mutex> lock(aq.mutex);
		if (!aq.q.empty())
			return false;
	}
	if (audio_dev_ && SDL_GetQueuedAudioSize(audio_dev_) > 0)
		return false;
	return true;
}

// ---------------------------------------------------------------------------
// Pipeline start / stop
// ---------------------------------------------------------------------------

bool Player::OpenAudioDevice()
{
	// SDL queue mode (no callback). The amount of queued but not yet played
	// audio is what drives the master clock.
	SDL_AudioSpec want = {}, have = {};
	want.freq = kAudioRate;
	want.format = AUDIO_S16SYS;
	want.channels = kAudioChannels;
	want.samples = 1024;
	audio_dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
	if (!audio_dev_)
	{
		std::fprintf(stderr, "audio device unavailable (%s); using wall clock\n", SDL_GetError());
		return false;
	}
	SDL_PauseAudioDevice(audio_dev_, 0);
	return true;
}

bool Player::StartPipeline()
{
	if (audio_stream_ >= 0 && !OpenAudioDevice())
	{
		avcodec_free_context(&actx_);
		audio_stream_ = -1;
	}

	audio_pts_end_ = INT64_MIN;
	wall_anchor_pts_ = INT64_MIN;
	paused_ = false;
	eof_ = false;

	threads_running_ = true;
	if (video_stream_ >= 0)
		vthread_ = std::thread(&Player::VideoThread, this);
	if (audio_stream_ >= 0)
		athread_ = std::thread(&Player::AudioThread, this);
	return video_stream_ >= 0 || audio_stream_ >= 0;
}

void Player::StopPipeline()
{
	threads_running_ = false;
	paused_ = false;
	vqueue_.cv.notify_all();
	aqueue_.cv.notify_all();
	frames_cv_.notify_all();

	if (dthread_.joinable())
		dthread_.join();
	if (vthread_.joinable())
		vthread_.join();
	if (athread_.joinable())
		athread_.join();

	vqueue_.Clear();
	aqueue_.Clear();
	DropDecodedFrames();
	visualizer_.Reset();

	if (audio_dev_)
	{
		SDL_CloseAudioDevice(audio_dev_);
		audio_dev_ = 0;
	}
	if (vctx_) avcodec_free_context(&vctx_);
	if (actx_) avcodec_free_context(&actx_);
	if (sws_) { sws_freeContext(sws_); sws_ = nullptr; }
	resampler_.Reset();
}

void Player::DropDecodedFrames()
{
	std::lock_guard<std::mutex> lock(frames_mutex_);
	for (TimedFrame& tf : frames_)
		av_frame_free(&tf.frame);
	frames_.clear();
	frames_cv_.notify_all();
}

// ---------------------------------------------------------------------------
// Clock
// ---------------------------------------------------------------------------

int64_t Player::MasterClock() const
{
	const int64_t aend = audio_pts_end_;
	if (audio_dev_ && aend != INT64_MIN)
	{
		const int64_t queued_us = (int64_t)SDL_GetQueuedAudioSize(audio_dev_) * 1000000 / kAudioBytesPerSec;
		return aend - queued_us;
	}
	// No audio: wall clock anchored at the first video frame.
	const int64_t anchor = wall_anchor_pts_;
	if (anchor == INT64_MIN)
		return INT64_MIN;
	if (paused_)
		return anchor;
	return anchor + (av_gettime_relative() - wall_anchor_time_);
}

// ---------------------------------------------------------------------------
// Decode threads
// ---------------------------------------------------------------------------

void Player::PushVideoFrame(AVFrame* frame)
{
	if (frame->width <= 0 || frame->height <= 0 || frame->format < 0)
		return;

	// Normalize to yuv420p (= SDL IYUV) if the decoder emits anything else.
	// sws_getCachedContext rebuilds itself when the size or pixel format
	// changes, which a file can do mid-stream.
	if (frame->format != AV_PIX_FMT_YUV420P)
	{
		AVFrame* conv = av_frame_alloc();
		if (!conv)
			return;
		conv->format = AV_PIX_FMT_YUV420P;
		conv->width = frame->width;
		conv->height = frame->height;
		sws_ = sws_getCachedContext(sws_, frame->width, frame->height, (AVPixelFormat)frame->format,
			frame->width, frame->height, AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
		if (!sws_ || av_frame_get_buffer(conv, 0) < 0)
		{
			av_frame_free(&conv);
			return;
		}
		sws_scale(sws_, frame->data, frame->linesize, 0, frame->height, conv->data, conv->linesize);
		conv->pts = frame->pts;
		conv->best_effort_timestamp = frame->best_effort_timestamp;
		conv->sample_aspect_ratio = frame->sample_aspect_ratio;
		av_frame_unref(frame);
		av_frame_move_ref(frame, conv);
		av_frame_free(&conv);
	}

	TimedFrame tf;
	tf.frame = av_frame_alloc();
	if (!tf.frame)
		return;
	av_frame_move_ref(tf.frame, frame);
	tf.pts = (tf.frame->best_effort_timestamp != AV_NOPTS_VALUE) ? tf.frame->best_effort_timestamp : INT64_MIN;

	std::unique_lock<std::mutex> lock(frames_mutex_);
	frames_cv_.wait(lock, [&] { return frames_.size() < kMaxFrames || !threads_running_; });
	if (!threads_running_)
	{
		av_frame_free(&tf.frame);
		return;
	}
	frames_.push_back(tf);
}

void Player::VideoThread()
{
	AVPacket* pkt = av_packet_alloc();
	AVFrame* frame = av_frame_alloc();
	uint32_t epoch = 0;
	bool have_epoch = false;

	int ctx_stream = video_stream_.load(); // stream vctx_ was opened for
	// After a track switch the first packets are usually mid-GOP, and
	// feeding those to a fresh decoder yields nothing but green mush.
	// Wait for a keyframe, but not forever: a container that does not flag
	// them would otherwise stall the picture for good.
	int skip_budget = 0;

	DemuxPacket in;
	uint32_t pkt_epoch = 0;
	while (vqueue_.Pop(in, threads_running_, &pkt_epoch))
	{
		// A flush happened (seek): drop whatever the decoder is holding.
		if (!have_epoch)
		{
			epoch = pkt_epoch;
			have_epoch = true;
		}
		else if (pkt_epoch != epoch)
		{
			epoch = pkt_epoch;
			avcodec_flush_buffers(vctx_);
		}

		// Track switch: the demuxer now queues a different stream, so the
		// decoder has to be rebuilt for it. On failure the packet is
		// dropped and the reopen retried on the next one.
		if ((int)in.stream != ctx_stream)
		{
			if (!ReopenVideoDecoder((int)in.stream))
				continue;
			ctx_stream = (int)in.stream;
			skip_budget = 120;
		}
		if (skip_budget > 0)
		{
			if (in.frametype != 'I' && --skip_budget > 0)
				continue;
			skip_budget = 0;
		}

		av_new_packet(pkt, (int)in.payload.size());
		std::memcpy(pkt->data, in.payload.data(), in.payload.size());
		pkt->pts = (in.pts != INT64_MIN) ? in.pts : AV_NOPTS_VALUE;
		pkt->dts = (in.dts != INT64_MIN) ? in.dts : AV_NOPTS_VALUE;
		if (in.frametype == 'I')
			pkt->flags |= AV_PKT_FLAG_KEY;

		if (avcodec_send_packet(vctx_, pkt) == 0)
		{
			while (avcodec_receive_frame(vctx_, frame) == 0)
			{
				// No audio stream: anchor the wall clock at the first frame.
				if (audio_stream_ < 0 && wall_anchor_pts_ == INT64_MIN &&
				    frame->best_effort_timestamp != AV_NOPTS_VALUE)
				{
					wall_anchor_pts_ = frame->best_effort_timestamp;
					wall_anchor_time_ = av_gettime_relative();
				}
				PushVideoFrame(frame);
				av_frame_unref(frame);
			}
		}
		av_packet_unref(pkt);
	}

	av_frame_free(&frame);
	av_packet_free(&pkt);
}

bool Player::ReopenVideoDecoder(int stream_index)
{
	if (!fmt_ || stream_index < 0 || stream_index >= (int)fmt_->nb_streams)
		return false;
	AVStream* st = fmt_->streams[stream_index];
	const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
	if (!codec)
		return false;
	AVCodecContext* ctx = avcodec_alloc_context3(codec);
	if (!ctx)
		return false;
	ctx->thread_count = 0;
	if (avcodec_parameters_to_context(ctx, st->codecpar) < 0 ||
	    avcodec_open2(ctx, codec, nullptr) < 0)
	{
		avcodec_free_context(&ctx);
		return false;
	}
	ctx->pkt_timebase = st->time_base;

	avcodec_free_context(&vctx_);
	vctx_ = ctx;
	// The new track may have a different size or pixel format; the scaler
	// is rebuilt on the next frame that needs one.
	if (sws_)
	{
		sws_freeContext(sws_);
		sws_ = nullptr;
	}
	return true;
}

bool Player::ReopenAudioDecoder(int stream_index)
{
	if (!fmt_ || stream_index < 0 || stream_index >= (int)fmt_->nb_streams)
		return false;
	AVStream* st = fmt_->streams[stream_index];
	const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
	if (!codec)
		return false;
	AVCodecContext* ctx = avcodec_alloc_context3(codec);
	if (!ctx)
		return false;
	if (avcodec_parameters_to_context(ctx, st->codecpar) < 0 ||
	    avcodec_open2(ctx, codec, nullptr) < 0)
	{
		avcodec_free_context(&ctx);
		return false;
	}
	ctx->pkt_timebase = st->time_base;

	avcodec_free_context(&actx_);
	actx_ = ctx;
	return true;
}

void Player::AudioThread()
{
	AVPacket* pkt = av_packet_alloc();
	AVFrame* frame = av_frame_alloc();
	std::vector<uint8_t> out;
	int64_t pts_accum = INT64_MIN;
	uint32_t epoch = 0;
	bool have_epoch = false;
	int ctx_stream = audio_stream_.load(); // stream actx_ was opened for

	DemuxPacket in;
	uint32_t pkt_epoch = 0;
	while (aqueue_.Pop(in, threads_running_, &pkt_epoch))
	{
		if (!have_epoch)
		{
			epoch = pkt_epoch;
			have_epoch = true;
		}
		else if (pkt_epoch != epoch)
		{
			// Seek: drop the decoder's and the resampler's buffered tail so
			// nothing from before the jump is played afterwards.
			epoch = pkt_epoch;
			pts_accum = INT64_MIN;
			avcodec_flush_buffers(actx_);
			resampler_.Reset();
			visualizer_.Reset();
		}

		// Track switch: the demuxer now queues a different stream, so the
		// decoder has to be rebuilt for it. On failure the packet is
		// dropped and the reopen retried on the next one.
		if ((int)in.stream != ctx_stream)
		{
			if (!ReopenAudioDecoder((int)in.stream))
				continue;
			ctx_stream = (int)in.stream;
			pts_accum = INT64_MIN;
			resampler_.Reset();
			visualizer_.Reset();
		}

		av_new_packet(pkt, (int)in.payload.size());
		std::memcpy(pkt->data, in.payload.data(), in.payload.size());
		pkt->pts = (in.pts != INT64_MIN) ? in.pts : AV_NOPTS_VALUE;
		pkt->dts = (in.dts != INT64_MIN) ? in.dts : AV_NOPTS_VALUE;

		if (avcodec_send_packet(actx_, pkt) == 0)
		{
			while (avcodec_receive_frame(actx_, frame) == 0)
			{
				const int got = resampler_.Convert(frame, out);
				if (got < 0)
				{
					// Unconvertible frame (bad format, or the resampler could
					// not be built for it): skip it rather than stopping.
					av_frame_unref(frame);
					continue;
				}
				if (got > 0 && audio_dev_)
				{
					// Keep at most ~2s of audio queued; otherwise an
					// audio-only file is slurped whole into the SDL queue
					// and pause/seek stop feeling immediate.
					//
					// Pausing has to block here too. The device stops
					// draining while paused, so skipping the throttle
					// meant the rest of the buffered stream went into the
					// queue in one go and the decoder ended up seconds
					// ahead of playback -- 20+ seconds for formats with
					// large frames, such as FLAC.
					while (threads_running_ &&
					       (paused_ ||
					        SDL_GetQueuedAudioSize(audio_dev_) > (Uint32)(2 * kAudioBytesPerSec)))
						std::this_thread::sleep_for(std::chrono::milliseconds(50));
					if (!threads_running_)
					{
						av_frame_unref(frame);
						break;
					}
					// A seek may have flushed the queue while this frame
					// waited above. It is from before the jump, so drop it
					// and pick up the new packets instead of playing a
					// stale one after the seek.
					if (aqueue_.Epoch() != epoch)
					{
						av_frame_unref(frame);
						break;
					}

					SDL_QueueAudio(audio_dev_, out.data(), (Uint32)got * kAudioChannels * 2);

					const int64_t dur = (int64_t)frame->nb_samples * 1000000 / frame->sample_rate;
					if (frame->best_effort_timestamp != AV_NOPTS_VALUE)
						pts_accum = frame->best_effort_timestamp + dur;
					else if (pts_accum != INT64_MIN)
						pts_accum += dur;
					if (pts_accum != INT64_MIN)
						audio_pts_end_ = pts_accum;

					// Same block, same timestamp the master clock counts
					// down from, so the animation lands on what is audible
					// rather than on what was last decoded.
					visualizer_.Push((const int16_t*)out.data(), got, pts_accum);
				}
				av_frame_unref(frame);
			}
		}
		av_packet_unref(pkt);
	}

	av_frame_free(&frame);
	av_packet_free(&pkt);
}

// ---------------------------------------------------------------------------
// Presentation
// ---------------------------------------------------------------------------

void Player::RenderVideo(int win_w, int win_h)
{
	if (!renderer_)
		return;

	// Audio-only: no frame will ever arrive, and the cleared backbuffer would
	// stay black behind the now-playing card. Draw the spectrum instead.
	if (video_stream_ < 0)
	{
		if (active_ || threads_running_)
			visualizer_.Render(renderer_, win_w, win_h, MasterClock(), paused_);
		return;
	}

	if (!threads_running_ && !current_.frame)
		return;

	// Advance to the newest frame that is due (drop older late ones).
	const int64_t clock = MasterClock();
	{
		std::lock_guard<std::mutex> lock(frames_mutex_);
		while (!frames_.empty())
		{
			const TimedFrame& head = frames_.front();
			const bool due = (head.pts == INT64_MIN) || (clock != INT64_MIN && head.pts <= clock);
			if (!due)
				break;
			if (current_.frame)
				av_frame_free(&current_.frame);
			current_ = frames_.front();
			frames_.pop_front();
			frames_cv_.notify_one();
		}
	}

	if (current_.pts != INT64_MIN)
		last_position_us_ = current_.pts;

	if (!current_.frame)
		return;
	AVFrame* f = current_.frame;

	// (Re)create the streaming texture when the frame size changes. IYUV is
	// exactly yuv420p, which PushVideoFrame() normalizes to.
	if (!texture_ || tex_w_ != f->width || tex_h_ != f->height)
	{
		if (texture_)
			SDL_DestroyTexture(texture_);
		texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, f->width, f->height);
		if (!texture_)
		{
			std::fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
			return;
		}
		SDL_SetTextureBlendMode(texture_, SDL_BLENDMODE_NONE);
		tex_w_ = f->width;
		tex_h_ = f->height;
	}

	SDL_UpdateYUVTexture(texture_, nullptr,
		f->data[0], f->linesize[0],
		f->data[1], f->linesize[1],
		f->data[2], f->linesize[2]);

	// Letterbox: fit the (SAR-corrected) frame into the window.
	double dar = (double)f->width / f->height;
	if (f->sample_aspect_ratio.num > 0 && f->sample_aspect_ratio.den > 0)
		dar *= av_q2d(f->sample_aspect_ratio);
	const double win_ar = (double)win_w / win_h;
	SDL_Rect dst;
	if (win_ar > dar)
	{
		dst.h = win_h;
		dst.w = (int)(win_h * dar + 0.5);
	}
	else
	{
		dst.w = win_w;
		dst.h = (int)(win_w / dar + 0.5);
	}
	dst.x = (win_w - dst.w) / 2;
	dst.y = (win_h - dst.h) / 2;

	SDL_RenderCopy(renderer_, texture_, nullptr, &dst);
}

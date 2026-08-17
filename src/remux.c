/* Copyright (C) 2026 John Törnblom

This program is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation; either version 3, or (at your option) any
later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; see the file COPYING. If not, see
<http://www.gnu.org/licenses/>.  */

#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <microhttpd.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>

#include "remux.h"


/**
 * Segments are cut on the source's own keyframes, so this is what a cut aims
 * for rather than what it gets; a source with a 10 second gop yields 10
 * second segments no matter what is asked for here.
 **/
#define REMUX_SEG_TARGET 6.0

/**
 * A trailing segment shorter than this is folded into the one before it
 * rather than left as a sliver of its own.
 **/
#define REMUX_SEG_MIN 1.0

/**
 * Sources with more streams than this are not something the console is going
 * to play anyway, and the cap keeps the input to output map a plain array.
 **/
#define REMUX_MAX_STREAMS 64

/**
 * Buffer sizes for the muxed output, in bytes.
 **/
#define REMUX_AVIO_SIZE  (16 * 1024)
#define REMUX_BLOCK_SIZE (32 * 1024)

/**
 * Every segment is muxed by its own muxer, but they all have to land on one
 * timeline for the player to treat them as one recording, so timestamps keep
 * their distance from the start of the source. The constant offset on top of
 * that keeps the first segment clear of zero, which is what ffmpeg's own
 * muxer does, with the same value.
 **/
#define REMUX_TS_BASE 1400000

/**
 * Room for a percent encoded uri, which is at worst three characters per byte.
 **/
#define REMUX_URI_MAX (PATH_MAX * 3)

/**
 * libavformat made the avio write callback const in 7.0.
 **/
#if LIBAVFORMAT_VERSION_MAJOR >= 61
#define REMUX_AVIO_BUF const uint8_t*
#else
#define REMUX_AVIO_BUF uint8_t*
#endif

/**
 * The index accessors arrived in libavformat 59; without them a source cannot
 * be cut into segments and everything falls back to a single stream.
 **/
#if LIBAVFORMAT_VERSION_MAJOR >= 59
#define REMUX_HAVE_INDEX_API 1
#endif


/**
 * What the source can be rewrapped as.
 *
 * The console plays h264 and h265 alongside aac and ac3 out of mp4 and out of
 * mpeg-ts, and vp9 alongside opus out of webm. The two sets do not mix: there
 * is no wrapper that holds vp9 and aac in a way the browser will take, so a
 * source that straddles them needs a transcode rather than a remux.
 **/
typedef enum remux_mode {
  REMUX_MODE_NONE = 0, /* no wrapper works; the codecs have to change */
  REMUX_MODE_TS,       /* h264/h265 + aac/ac3, served as hls or fragmented mp4 */
  REMUX_MODE_WEBM      /* vp9 + opus, served as webm */
} remux_mode_t;


/**
 * What a source holds and what can be done about it.
 **/
typedef struct remux_plan {
  remux_mode_t mode;
  int      vstream;   /* input stream index, or -1 */
  int      astream;   /* input stream index, or -1 */
  int      seekable;  /* the source can be seeked, so segments are possible */
  int      direct;    /* the browser would have played the source untouched */
  double   duration;  /* seconds, or 0 when unknown */
  int64_t  start;     /* first timestamp, AV_TIME_BASE units */
  char     reason[192];
} remux_plan_t;


/**
 * One segment of an hls rendition.
 **/
typedef struct remux_seg {
  double start;
  double dur;
} remux_seg_t;


/**
 * A growable text buffer, for playlists and json whose length is not known
 * before they are written.
 **/
typedef struct remux_buf {
  char  *ptr;
  size_t len;
  size_t cap;
} remux_buf_t;


/**
 * State of one remuxed stream in flight.
 **/
typedef struct remux {
  AVFormatContext *ifmt;
  AVFormatContext *ofmt;
  AVPacket        *pkt;

  int     map[REMUX_MAX_STREAMS];      /* input stream index -> output, or -1 */
  int     done[REMUX_MAX_STREAMS];     /* by output stream index */
  int64_t last_dts[REMUX_MAX_STREAMS]; /* by output stream index */
  int     seen[REMUX_MAX_STREAMS];     /* a packet has been emitted */
  int     derive[REMUX_MAX_STREAMS];   /* leave decode timestamps to the muxer */
  int     nb_out;

  /* the window to emit, relative to the start of the source, in AV_TIME_BASE
     units; the whole source is start = INT64_MIN/2, end = INT64_MAX */
  int64_t start_us;
  int64_t end_us;
  int64_t vend_us; /* hard stop for video, should the boundary keyframe not come */
  int64_t vkey_t;  /* when the segment's opening keyframe presents */
  int     vstarted;/* the opening keyframe of the segment has been seen */
  int64_t vpackets;/* video packets emitted, so the opening keyframe cannot also close */
  int64_t t0;      /* where the source starts, so the window can be relative */
  int64_t base_us; /* constant offset added to every output timestamp */

  /* muxed bytes waiting to be handed to libmicrohttpd */
  uint8_t *out;
  size_t   out_len;
  size_t   out_cap;
  int      eof;
} remux_t;


/**
 * Internal server error (500)
 **/
#define PAGE_500                                \
  "<html>"                                      \
  "  <head>"                                    \
  "    <title>Internal server error</title>"     \
  "  </head>"                                   \
  "  <body>Internal server error</body>"        \
  "</html>"


/**
 * Append to a growable buffer.
 **/
static int
remux_buf_addf(remux_buf_t *b, const char *fmt, ...) {
  va_list ap;
  int n;

  while(1) {
    va_start(ap, fmt);
    n = vsnprintf(b->ptr ? b->ptr + b->len : 0,
                  b->ptr ? b->cap - b->len : 0, fmt, ap);
    va_end(ap);

    if(n < 0) {
      return -1;
    }
    if(b->ptr && (size_t)n < b->cap - b->len) {
      b->len += n;
      return 0;
    }

    size_t cap = b->cap ? b->cap * 2 : 4096;
    while(cap - b->len <= (size_t)n) {
      cap *= 2;
    }

    char *ptr = realloc(b->ptr, cap);
    if(!ptr) {
      return -1;
    }
    b->ptr = ptr;
    b->cap = cap;
  }
}


static void
remux_buf_free(remux_buf_t *b) {
  free(b->ptr);
  b->ptr = 0;
  b->len = 0;
  b->cap = 0;
}


/**
 * Percent encode a uri so it survives a round trip through a query argument.
 **/
static int
remux_uriencode(const char* in, char* out, size_t outsize) {
  static const char hex[] = "0123456789ABCDEF";
  size_t pos = 0;
  uint8_t c;

  if(!in || !out || !outsize) {
    return -1;
  }

  while(*in) {
    c = (uint8_t)*in++;

    if((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
       (c >= '0' && c <= '9') || c == '-' || c == '_' ||
       c == '.' || c == '~') {
      if(pos + 1 >= outsize) {
        return -1;
      }
      out[pos++] = (char)c;
    } else {
      if(pos + 3 >= outsize) {
        return -1;
      }
      out[pos++] = '%';
      out[pos++] = hex[c >> 4];
      out[pos++] = hex[c & 0x0F];
    }
  }

  out[pos] = '\0';

  return (int)pos;
}


/**
 * Escape a string so it can go inside a json string literal.
 **/
static void
remux_jsonescape(const char* in, char* out, size_t outsize) {
  size_t pos = 0;
  uint8_t c;

  if(!out || !outsize) {
    return;
  }

  while(in && *in && pos + 7 < outsize) {
    c = (uint8_t)*in++;

    switch(c) {
    case '"':  out[pos++] = '\\'; out[pos++] = '"';  break;
    case '\\': out[pos++] = '\\'; out[pos++] = '\\'; break;
    case '\n': out[pos++] = '\\'; out[pos++] = 'n';  break;
    case '\r': out[pos++] = '\\'; out[pos++] = 'r';  break;
    case '\t': out[pos++] = '\\'; out[pos++] = 't';  break;
    default:
      if(c < 0x20) {
        pos += snprintf(out + pos, outsize - pos, "\\u%04x", c);
      } else {
        out[pos++] = (char)c;
      }
    }
  }

  out[pos] = '\0';
}


/**
 * Lowercased file extension of a uri, without the dot.
 **/
static void
remux_uriext(const char* uri, char* out, size_t outsize) {
  const char *end;
  const char *dot = 0;
  const char *p;
  size_t n = 0;

  out[0] = '\0';
  if(!uri) {
    return;
  }

  end = strpbrk(uri, "?#");
  if(!end) {
    end = uri + strlen(uri);
  }

  for(p=end; p>uri; p--) {
    if(p[-1] == '/') {
      break;
    }
    if(p[-1] == '.' && !dot) {
      dot = p;
    }
  }

  if(!dot) {
    return;
  }

  for(p=dot; p<end && n+1<outsize; p++) {
    out[n++] = (*p >= 'A' && *p <= 'Z') ? (char)(*p - 'A' + 'a') : *p;
  }
  out[n] = '\0';
}


/**
 * Codecs the console decodes, grouped by the wrapper that can carry them.
 **/
static int
remux_video_ok(enum AVCodecID id, remux_mode_t mode) {
  switch(mode) {
  case REMUX_MODE_TS:   return id == AV_CODEC_ID_H264 || id == AV_CODEC_ID_HEVC;
  case REMUX_MODE_WEBM: return id == AV_CODEC_ID_VP9;
  default:              return 0;
  }
}


static int
remux_audio_ok(enum AVCodecID id, remux_mode_t mode) {
  switch(mode) {
  case REMUX_MODE_TS:   return id == AV_CODEC_ID_AAC || id == AV_CODEC_ID_AC3;
  case REMUX_MODE_WEBM: return id == AV_CODEC_ID_OPUS;
  default:              return 0;
  }
}


/**
 * The wrapper a codec asks for, if any.
 **/
static remux_mode_t
remux_video_mode(enum AVCodecID id) {
  if(remux_video_ok(id, REMUX_MODE_TS)) {
    return REMUX_MODE_TS;
  }
  if(remux_video_ok(id, REMUX_MODE_WEBM)) {
    return REMUX_MODE_WEBM;
  }
  return REMUX_MODE_NONE;
}


static remux_mode_t
remux_audio_mode(enum AVCodecID id) {
  if(remux_audio_ok(id, REMUX_MODE_TS)) {
    return REMUX_MODE_TS;
  }
  if(remux_audio_ok(id, REMUX_MODE_WEBM)) {
    return REMUX_MODE_WEBM;
  }
  return REMUX_MODE_NONE;
}


/**
 * Pick the video stream, preferring the largest and passing over cover art,
 * which matroska and mp4 both carry as a video stream of one still frame.
 **/
static int
remux_find_video(AVFormatContext *ic) {
  int best = -1;

  for(unsigned i=0; i<ic->nb_streams && i<REMUX_MAX_STREAMS; i++) {
    AVCodecParameters *cp = ic->streams[i]->codecpar;

    if(cp->codec_type != AVMEDIA_TYPE_VIDEO) {
      continue;
    }
    if(ic->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC) {
      continue;
    }
    if(best < 0 || cp->width * cp->height >
       ic->streams[best]->codecpar->width * ic->streams[best]->codecpar->height) {
      best = (int)i;
    }
  }

  return best;
}


/**
 * Pick the audio stream, preferring one the chosen wrapper can carry. Files
 * with a dts or truehd track next to an ac3 one are common enough that taking
 * libavformat's favourite without looking would turn a playable source into
 * an unplayable one.
 **/
static int
remux_find_audio(AVFormatContext *ic, remux_mode_t mode) {
  int best = av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO, -1, -1, 0, 0);

  if(best >= 0 && best < REMUX_MAX_STREAMS &&
     remux_audio_ok(ic->streams[best]->codecpar->codec_id, mode)) {
    return best;
  }

  for(unsigned i=0; i<ic->nb_streams && i<REMUX_MAX_STREAMS; i++) {
    AVCodecParameters *cp = ic->streams[i]->codecpar;

    if(cp->codec_type == AVMEDIA_TYPE_AUDIO &&
       remux_audio_ok(cp->codec_id, mode)) {
      return (int)i;
    }
  }

  return best < REMUX_MAX_STREAMS ? best : -1;
}


/**
 * Open a source. Anything libavformat can open works; file, http and https
 * are the ones that matter.
 **/
static int
remux_open_input(AVFormatContext **ic, const char* uri) {
  AVDictionary *opts = 0;
  int ret;

  if(!(*ic=avformat_alloc_context())) {
    return AVERROR(ENOMEM);
  }

  /* avi and some mpeg-ts captures carry decode timestamps only, and the
     muxers downstream want both */
  (*ic)->flags |= AVFMT_FLAG_GENPTS;

  av_dict_set(&opts, "user_agent", "jtplay", 0);
  av_dict_set(&opts, "reconnect", "1", 0);
  av_dict_set(&opts, "reconnect_streamed", "1", 0);

  ret = avformat_open_input(ic, uri, 0, &opts);
  av_dict_free(&opts);
  if(ret < 0) {
    return ret;
  }

  if((ret=avformat_find_stream_info(*ic, 0)) < 0) {
    return ret;
  }

  return 0;
}


/**
 * Work out what the source holds and what it can be rewrapped as.
 **/
static void
remux_plan(AVFormatContext *ic, const char* uri, remux_plan_t *p) {
  char ext[16];

  memset(p, 0, sizeof(*p));
  p->vstream = remux_find_video(ic);
  p->astream = -1;
  p->start = ic->start_time != AV_NOPTS_VALUE ? ic->start_time : 0;

  if(ic->duration != AV_NOPTS_VALUE && ic->duration > 0) {
    p->duration = (double)ic->duration / AV_TIME_BASE;
  }
  p->seekable = ic->pb && ic->pb->seekable;

  if(p->vstream >= 0) {
    enum AVCodecID vid = ic->streams[p->vstream]->codecpar->codec_id;

    p->mode = remux_video_mode(vid);
    if(p->mode == REMUX_MODE_NONE) {
      snprintf(p->reason, sizeof(p->reason),
               "%s video has to be transcoded, the browser cannot decode it",
               avcodec_get_name(vid));
      return;
    }

    p->astream = remux_find_audio(ic, p->mode);
    if(p->astream >= 0) {
      enum AVCodecID aid = ic->streams[p->astream]->codecpar->codec_id;

      if(!remux_audio_ok(aid, p->mode)) {
        snprintf(p->reason, sizeof(p->reason),
                 "%s audio has to be transcoded to sit alongside %s",
                 avcodec_get_name(aid), avcodec_get_name(vid));
        p->mode = REMUX_MODE_NONE;
        return;
      }
    }

  } else {
    p->astream = av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO, -1, -1, 0, 0);
    if(p->astream < 0 || p->astream >= REMUX_MAX_STREAMS) {
      p->astream = -1;
      snprintf(p->reason, sizeof(p->reason),
               "no audio or video stream to remux");
      p->mode = REMUX_MODE_NONE;
      return;
    }

    enum AVCodecID aid = ic->streams[p->astream]->codecpar->codec_id;
    p->mode = remux_audio_mode(aid);
    if(p->mode == REMUX_MODE_NONE) {
      snprintf(p->reason, sizeof(p->reason),
               "%s audio has to be transcoded, the browser cannot decode it",
               avcodec_get_name(aid));
      return;
    }
  }

  /* Whether the browser would have taken the source untouched. The demuxer
     cannot tell matroska from webm, so the extension is what is left to go
     on; being wrong here only costs a remux that was not needed. */
  remux_uriext(uri, ext, sizeof(ext));
  if(p->mode == REMUX_MODE_TS) {
    p->direct = !strcmp(ext, "mp4") || !strcmp(ext, "m4v") ||
                !strcmp(ext, "m4a") || !strcmp(ext, "m3u8");
  } else if(p->mode == REMUX_MODE_WEBM) {
    p->direct = !strcmp(ext, "webm");
  }
}


/**
 * Collect the keyframe timestamps of a stream, in seconds from the start of
 * the source.
 *
 * Containers that carry an index hand this over for free: matroska has its
 * cues, avi its idx1, mp4 its sync sample table. Matroska parses the cues on
 * the first seek rather than at open, so a seek is what asks for them. Note
 * that avi and mp4 index every frame, not just the keyframes, hence the flag
 * test.
 **/
static int
remux_keyframes(AVFormatContext *ic, int sidx, double t0, double **out) {
#ifdef REMUX_HAVE_INDEX_API
  AVStream *st = ic->streams[sidx];
  double *ks;
  int n;
  int m = 0;

  av_seek_frame(ic, sidx, 0, AVSEEK_FLAG_BACKWARD);

  if((n=avformat_index_get_entries_count(st)) < 2) {
    return 0;
  }

  if(!(ks=calloc(n, sizeof(double)))) {
    return 0;
  }

  for(int i=0; i<n; i++) {
    const AVIndexEntry *e = avformat_index_get_entry(st, i);
    double t;

    if(!e || !(e->flags & AVINDEX_KEYFRAME)) {
      continue;
    }

    t = e->timestamp * av_q2d(st->time_base) - t0;
    if(t < 0) {
      t = 0;
    }
    if(m && t <= ks[m-1] + 0.001) {
      continue;
    }
    ks[m++] = t;
  }

  if(m < 2) {
    free(ks);
    return 0;
  }

  *out = ks;
  return m;
#else
  (void)ic; (void)sidx; (void)t0; (void)out;
  return 0;
#endif
}


/**
 * Group keyframes into segments of roughly the target length.
 **/
static int
remux_segments(const double *ks, int n, double duration, remux_seg_t **out) {
  remux_seg_t *segs;
  double cur = 0;
  int m = 0;

  if(duration <= 0 || n < 2) {
    return 0;
  }

  if(!(segs=calloc(n + 1, sizeof(remux_seg_t)))) {
    return 0;
  }

  for(int i=1; i<n; i++) {
    if(ks[i] - cur >= REMUX_SEG_TARGET - 0.001 && ks[i] < duration - 0.001) {
      segs[m].start = cur;
      segs[m].dur = ks[i] - cur;
      m++;
      cur = ks[i];
    }
  }

  if(duration > cur + 0.001) {
    if(m && duration - cur < REMUX_SEG_MIN) {
      /* a sliver of a segment is more trouble to a player than a long one */
      segs[m-1].dur = duration - segs[m-1].start;
    } else {
      segs[m].start = cur;
      segs[m].dur = duration - cur;
      m++;
    }
  }

  if(!m) {
    free(segs);
    return 0;
  }

  *out = segs;
  return m;
}


/**
 * Append muxed bytes to the queue drained by libmicrohttpd.
 **/
static int
remux_avio_write(void *opaque, REMUX_AVIO_BUF buf, int size) {
  remux_t *r = (remux_t*)opaque;

  if(r->out_len + size > r->out_cap) {
    size_t cap = r->out_len + size + REMUX_BLOCK_SIZE;
    uint8_t *ptr = realloc(r->out, cap);

    if(!ptr) {
      return AVERROR(ENOMEM);
    }
    r->out = ptr;
    r->out_cap = cap;
  }

  memcpy(r->out + r->out_len, buf, size);
  r->out_len += size;

  return size;
}


/**
 * Carry one input stream over to the output untouched.
 **/
static int
remux_add_stream(remux_t *r, int sidx) {
  AVStream *is = r->ifmt->streams[sidx];
  AVStream *os;
  int ret;

  if(!(os=avformat_new_stream(r->ofmt, 0))) {
    return AVERROR(ENOMEM);
  }

  if((ret=avcodec_parameters_copy(os->codecpar, is->codecpar)) < 0) {
    return ret;
  }

  /* the tag belongs to the source's container, and the muxer picks its own */
  os->codecpar->codec_tag = 0;
  os->time_base = is->time_base;
  os->disposition = is->disposition;
  av_dict_copy(&os->metadata, is->metadata, 0);

  if(os->index >= REMUX_MAX_STREAMS) {
    return AVERROR(EINVAL);
  }

  r->map[sidx] = os->index;
  r->last_dts[os->index] = AV_NOPTS_VALUE;
  r->nb_out++;

  return 0;
}


/**
 * Set up a muxer that writes into the response queue.
 **/
static int
remux_open_output(remux_t *r, const remux_plan_t *p, const char* fmt) {
  uint8_t *avio_buf;
  int ret;

  for(int i=0; i<REMUX_MAX_STREAMS; i++) {
    r->map[i] = -1;
  }

  if((ret=avformat_alloc_output_context2(&r->ofmt, 0, fmt, 0)) < 0) {
    return ret;
  }

  if(!(avio_buf=av_malloc(REMUX_AVIO_SIZE))) {
    return AVERROR(ENOMEM);
  }

  r->ofmt->pb = avio_alloc_context(avio_buf, REMUX_AVIO_SIZE, 1, r, 0,
                                   remux_avio_write, 0);
  if(!r->ofmt->pb) {
    av_free(avio_buf);
    return AVERROR(ENOMEM);
  }
  r->ofmt->flags |= AVFMT_FLAG_CUSTOM_IO | AVFMT_FLAG_FLUSH_PACKETS;

  if(!strcmp(fmt, "mpegts")) {
    /* every segment is muxed on its own, so the timestamps handed in are
       already on the shared timeline and must be left alone */
    av_opt_set(r->ofmt->priv_data, "mpegts_copyts", "1", 0);

  } else if(!strcmp(fmt, "mp4")) {
    /* nothing can seek back to patch a header into a socket, so the moov goes
       out empty and the media follows as fragments */
    av_opt_set(r->ofmt->priv_data, "movflags",
               "frag_keyframe+empty_moov+default_base_moof", 0);

  } else if(!strcmp(fmt, "webm")) {
    /* same reason: no cues, no duration written back into the header */
    av_opt_set(r->ofmt->priv_data, "live", "1", 0);
  }

  if(p->vstream >= 0 && (ret=remux_add_stream(r, p->vstream)) < 0) {
    return ret;
  }
  if(p->astream >= 0 && (ret=remux_add_stream(r, p->astream)) < 0) {
    return ret;
  }

  return avformat_write_header(r->ofmt, 0);
}


static int
remux_all_done(remux_t *r) {
  for(int i=0; i<r->nb_out; i++) {
    if(!r->done[i]) {
      return 0;
    }
  }
  return 1;
}


/**
 * Read one packet from the source, put it on the output timeline, and hand it
 * to the muxer. Muxed bytes land in the output queue via the avio callback.
 **/
static void
remux_pump(remux_t *r) {
  AVStream *is;
  AVStream *os;
  int64_t shift;
  int64_t pts;
  int64_t t;
  int oidx;
  int past;

  if(av_read_frame(r->ifmt, r->pkt) < 0) {
    av_write_trailer(r->ofmt);
    r->eof = 1;
    return;
  }

  if(r->pkt->stream_index < 0 ||
     r->pkt->stream_index >= REMUX_MAX_STREAMS ||
     (oidx=r->map[r->pkt->stream_index]) < 0) {
    av_packet_unref(r->pkt);
    return;
  }

  /* A stream that has reached the end of the segment is finished with, and
     what follows it belongs to the next one. A reordered stream keeps handing
     out packets that present earlier than the cut for a while after it, so
     the test below cannot be asked a second time. */
  if(r->done[oidx]) {
    av_packet_unref(r->pkt);
    return;
  }

  is = r->ifmt->streams[r->pkt->stream_index];
  os = r->ofmt->streams[oidx];

  /* Where this packet sits relative to the start of the source. A packet that
     states no time at all cannot be placed, so it is treated as belonging
     wherever the window currently is rather than dropped. */
  pts = r->pkt->pts != AV_NOPTS_VALUE ? r->pkt->pts : r->pkt->dts;
  t = pts != AV_NOPTS_VALUE
    ? av_rescale_q(pts, is->time_base, AV_TIME_BASE_Q) - r->t0
    : r->start_us;

  /* Where the segment ends. Video stops on the keyframe that opens the next
     segment rather than on the clock, because that keyframe is exactly where
     the next segment's seek lands, and because it is the only boundary both
     agree on: a container that indexes decode timestamps hands out cut points
     a frame or so away from the presentation timestamps tested here, and
     cutting on the clock would drop that frame on every boundary. */
  if(is->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && r->end_us != INT64_MAX) {
    /* Video opens on the keyframe the segment was cut at, and everything the
       source hands over after it belongs here, including the frames that
       present before it: a reordered stream carries the opening frames of a
       group after the keyframe they depend on, and judging those by the clock
       would throw away a few frames at every boundary. */
    if(!r->vstarted) {
      if(!(r->pkt->flags & AV_PKT_FLAG_KEY) || t < r->start_us - 1000) {
        av_packet_unref(r->pkt);
        return;
      }
      r->vstarted = 1;
      r->vkey_t = t;
    }

    /* A group that is left open carries frames that follow its keyframe in
       decode order but present before it, and those lean on the group before
       this one, which a player starting here does not have. The codec calls
       them skipped leading frames and expects exactly that of anyone who
       starts at the keyframe, so they go no further. */
    if(t < r->vkey_t) {
      av_packet_unref(r->pkt);
      return;
    }

    past = (r->vpackets && (r->pkt->flags & AV_PKT_FLAG_KEY) &&
            t >= r->end_us - 1000) || t >= r->vend_us;
    r->vpackets++;

  } else {
    past = t >= r->end_us;

    /* the seek lands on a video keyframe, and the audio around it runs to its
       own boundaries, so it can arrive from before the segment starts */
    if(!past && t < r->start_us - 1000) {
      av_packet_unref(r->pkt);
      return;
    }
  }

  if(past) {
    r->done[oidx] = 1;
    av_packet_unref(r->pkt);
    if(remux_all_done(r)) {
      av_write_trailer(r->ofmt);
      r->eof = 1;
    }
    return;
  }

  shift = av_rescale_q(r->base_us - r->t0, AV_TIME_BASE_Q, os->time_base);

  if(r->pkt->pts != AV_NOPTS_VALUE) {
    r->pkt->pts = av_rescale_q(r->pkt->pts, is->time_base, os->time_base) + shift;
  }
  if(r->pkt->dts != AV_NOPTS_VALUE) {
    r->pkt->dts = av_rescale_q(r->pkt->dts, is->time_base, os->time_base) + shift;
  }
  r->pkt->duration = av_rescale_q(r->pkt->duration, is->time_base, os->time_base);
  r->pkt->stream_index = oidx;
  r->pkt->pos = -1;

  if(r->pkt->pts == AV_NOPTS_VALUE) {
    r->pkt->pts = r->pkt->dts;
  }

  /* A source that has just been seeked to cannot always state a decode
     timestamp for the first frame it hands back, having not seen what comes
     before it; matroska leaves it unset and the muxer works one out from the
     reordering delay. The frames after it do carry one, and it is lower than
     what the muxer just derived, so taking each as it comes would walk the
     stream backwards. One or the other has to be believed for the whole of a
     stream, and the muxer is the one that can always answer. */
  if(!r->seen[oidx]) {
    r->seen[oidx] = 1;
    r->derive[oidx] = r->pkt->dts == AV_NOPTS_VALUE;
  }
  if(r->derive[oidx]) {
    r->pkt->dts = AV_NOPTS_VALUE;
  }

  if(r->pkt->dts != AV_NOPTS_VALUE) {
    /* A muxer refuses a stream whose decode timestamps stand still or go
       backwards, which a source with a damaged index can produce. */
    if(r->last_dts[oidx] != AV_NOPTS_VALUE && r->pkt->dts <= r->last_dts[oidx]) {
      r->pkt->dts = r->last_dts[oidx] + 1;
    }
    if(r->pkt->pts != AV_NOPTS_VALUE && r->pkt->pts < r->pkt->dts) {
      r->pkt->pts = r->pkt->dts;
    }
    r->last_dts[oidx] = r->pkt->dts;
  }

  /* One packet the muxer will not take is not a reason to drop the rest of
     the stream on the floor; the viewer would rather have a glitch than an
     ending. */
  av_interleaved_write_frame(r->ofmt, r->pkt);

  av_packet_unref(r->pkt);
}


/**
 * Callback function used to transmit the remuxed stream to a http request.
 * libmicrohttpd runs this on the connection's own thread, so pumping the
 * pipeline here is what paces the whole thing.
 **/
static ssize_t
remux_read_cb(void *cls, uint64_t pos, char *buf, size_t max) {
  remux_t *r = (remux_t*)cls;
  size_t n;

  while(!r->out_len && !r->eof) {
    remux_pump(r);
  }

  if(!r->out_len) {
    return MHD_CONTENT_READER_END_OF_STREAM;
  }

  n = r->out_len < max ? r->out_len : max;
  memcpy(buf, r->out, n);
  r->out_len -= n;
  memmove(r->out, r->out + n, r->out_len);

  return n;
}


/**
 * Callback function used to release a stream that has been transmitted, or
 * whose client went away.
 **/
static void
remux_free_cb(void *cls) {
  remux_t *r = (remux_t*)cls;

  if(r->ofmt) {
    if(r->ofmt->pb) {
      av_freep(&r->ofmt->pb->buffer);
      avio_context_free(&r->ofmt->pb);
    }
    avformat_free_context(r->ofmt);
  }

  avformat_close_input(&r->ifmt);
  av_packet_free(&r->pkt);

  free(r->out);
  free(r);
}


/**
 * Respond with an error page.
 **/
static enum MHD_Result
remux_response_error(struct MHD_Connection *conn, unsigned int status,
                     const char* msg) {
  enum MHD_Result ret = MHD_NO;
  struct MHD_Response *resp;

  if(!(resp=MHD_create_response_from_buffer(strlen(msg), (void*)msg,
                                            MHD_RESPMEM_MUST_COPY))) {
    resp = MHD_create_response_from_buffer(strlen(PAGE_500), (void*)PAGE_500,
                                           MHD_RESPMEM_PERSISTENT);
    status = MHD_HTTP_INTERNAL_SERVER_ERROR;
  }

  if(resp) {
    MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE, "text/plain");
    MHD_add_response_header(resp, MHD_HTTP_HEADER_ACCESS_CONTROL_ALLOW_ORIGIN,
                            "*");
    ret = MHD_queue_response(conn, status, resp);
    MHD_destroy_response(resp);
  }

  return ret;
}


/**
 * Respond with a body of the given type.
 **/
static enum MHD_Result
remux_response_text(struct MHD_Connection *conn, const char* body, size_t len,
                    const char* type) {
  enum MHD_Result ret = MHD_NO;
  struct MHD_Response *resp;

  if((resp=MHD_create_response_from_buffer(len, (void*)body,
                                           MHD_RESPMEM_MUST_COPY))) {
    MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE, type);
    MHD_add_response_header(resp, MHD_HTTP_HEADER_CACHE_CONTROL, "no-cache");
    MHD_add_response_header(resp, MHD_HTTP_HEADER_ACCESS_CONTROL_ALLOW_ORIGIN,
                            "*");
    ret = MHD_queue_response(conn, MHD_HTTP_OK, resp);
    MHD_destroy_response(resp);
  }

  return ret;
}


/**
 * The uri of the source, as given by the caller.
 **/
static const char*
remux_arg_uri(struct MHD_Connection *conn) {
  const char *uri = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND,
                                                "uri");
  return uri && uri[0] ? uri : 0;
}


/**
 * Open a source and plan for it, responding with the failure if there is one.
 **/
static int
remux_prepare(struct MHD_Connection *conn, const char* uri,
              AVFormatContext **ic, remux_plan_t *plan,
              enum MHD_Result *res) {
  char err[AV_ERROR_MAX_STRING_SIZE] = {0};
  int ret;

  avformat_network_init();
  av_log_set_level(AV_LOG_ERROR);

  if((ret=remux_open_input(ic, uri)) < 0) {
    /* the context outlives a failure that came after it was allocated, such
       as a source that opens but holds nothing readable */
    avformat_close_input(ic);
    av_strerror(ret, err, sizeof(err));
    *res = remux_response_error(conn, MHD_HTTP_BAD_GATEWAY, err);
    return -1;
  }

  remux_plan(*ic, uri, plan);

  return 0;
}


/**
 * The url that plays a source, given what it holds.
 **/
static void
remux_play_url(const remux_plan_t *p, const char* enc, char* out,
               size_t outsize) {
  switch(p->mode) {
  case REMUX_MODE_WEBM:
    snprintf(out, outsize, "/remux/stream.webm?uri=%s", enc);
    break;
  case REMUX_MODE_TS:
    snprintf(out, outsize, "/remux/master.m3u8?uri=%s", enc);
    break;
  default:
    out[0] = '\0';
  }
}


/**
 * Report what a source holds, whether the browser could play it as it
 * stands, and the url that plays it.
 **/
static enum MHD_Result
remux_request_probe(struct MHD_Connection *conn) {
  char enc[REMUX_URI_MAX];
  char esc[REMUX_URI_MAX];
  char url[REMUX_URI_MAX + 64];
  AVFormatContext *ic = 0;
  enum MHD_Result res;
  remux_plan_t plan;
  remux_buf_t buf = {0};
  const char *uri;
  const char *mode;

  if(!(uri=remux_arg_uri(conn))) {
    return remux_response_error(conn, MHD_HTTP_BAD_REQUEST,
                                "missing the uri query argument\n");
  }

  if(remux_prepare(conn, uri, &ic, &plan, &res) < 0) {
    return res;
  }

  if(remux_uriencode(uri, enc, sizeof(enc)) < 0) {
    avformat_close_input(&ic);
    return remux_response_error(conn, MHD_HTTP_URI_TOO_LONG, "uri too long\n");
  }
  remux_play_url(&plan, enc, url, sizeof(url));

  switch(plan.mode) {
  case REMUX_MODE_TS:   mode = "hls";  break;
  case REMUX_MODE_WEBM: mode = "webm"; break;
  default:              mode = "none";
  }

  remux_jsonescape(uri, esc, sizeof(esc));
  remux_buf_addf(&buf, "{\n  \"uri\": \"%s\",\n", esc);
  remux_jsonescape(ic->iformat->name, esc, sizeof(esc));
  remux_buf_addf(&buf, "  \"container\": \"%s\",\n", esc);
  remux_buf_addf(&buf, "  \"duration\": %.3f,\n", plan.duration);
  remux_buf_addf(&buf, "  \"seekable\": %s,\n", plan.seekable ? "true" : "false");

  if(plan.vstream >= 0) {
    AVCodecParameters *cp = ic->streams[plan.vstream]->codecpar;

    remux_buf_addf(&buf, "  \"video\": {\"codec\": \"%s\", \"width\": %d, "
                   "\"height\": %d},\n", avcodec_get_name(cp->codec_id),
                   cp->width, cp->height);
  } else {
    remux_buf_addf(&buf, "  \"video\": null,\n");
  }

  if(plan.astream >= 0) {
    AVCodecParameters *cp = ic->streams[plan.astream]->codecpar;

    remux_buf_addf(&buf, "  \"audio\": {\"codec\": \"%s\", \"channels\": %d, "
                   "\"sample_rate\": %d},\n", avcodec_get_name(cp->codec_id),
                   cp->ch_layout.nb_channels, cp->sample_rate);
  } else {
    remux_buf_addf(&buf, "  \"audio\": null,\n");
  }

  remux_buf_addf(&buf, "  \"direct\": %s,\n", plan.direct ? "true" : "false");
  remux_buf_addf(&buf, "  \"mode\": \"%s\",\n", mode);

  remux_jsonescape(plan.reason, esc, sizeof(esc));
  remux_buf_addf(&buf, "  \"reason\": \"%s\",\n", esc);
  remux_jsonescape(url, esc, sizeof(esc));
  remux_buf_addf(&buf, "  \"url\": \"%s\"\n}\n", esc);

  avformat_close_input(&ic);

  if(!buf.ptr) {
    return remux_response_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                                "out of memory\n");
  }

  res = remux_response_text(conn, buf.ptr, buf.len, "application/json");
  remux_buf_free(&buf);

  return res;
}


/**
 * Redirect to whichever endpoint suits the source, so a caller that does not
 * want to know any of this has one url to use.
 **/
static enum MHD_Result
remux_request_play(struct MHD_Connection *conn) {
  char enc[REMUX_URI_MAX];
  char url[REMUX_URI_MAX + 64];
  AVFormatContext *ic = 0;
  struct MHD_Response *resp;
  enum MHD_Result res;
  remux_plan_t plan;
  const char *uri;

  if(!(uri=remux_arg_uri(conn))) {
    return remux_response_error(conn, MHD_HTTP_BAD_REQUEST,
                                "missing the uri query argument\n");
  }

  if(remux_prepare(conn, uri, &ic, &plan, &res) < 0) {
    return res;
  }

  if(remux_uriencode(uri, enc, sizeof(enc)) < 0) {
    avformat_close_input(&ic);
    return remux_response_error(conn, MHD_HTTP_URI_TOO_LONG, "uri too long\n");
  }
  remux_play_url(&plan, enc, url, sizeof(url));
  avformat_close_input(&ic);

  if(!url[0]) {
    return remux_response_error(conn, MHD_HTTP_UNSUPPORTED_MEDIA_TYPE,
                                plan.reason);
  }

  if(!(resp=MHD_create_response_from_buffer(0, "", MHD_RESPMEM_PERSISTENT))) {
    return remux_response_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                                "out of memory\n");
  }

  MHD_add_response_header(resp, MHD_HTTP_HEADER_LOCATION, url);
  MHD_add_response_header(resp, MHD_HTTP_HEADER_CACHE_CONTROL, "no-cache");
  MHD_add_response_header(resp, MHD_HTTP_HEADER_ACCESS_CONTROL_ALLOW_ORIGIN,
                          "*");
  res = MHD_queue_response(conn, MHD_HTTP_FOUND, resp);
  MHD_destroy_response(resp);

  return res;
}


/**
 * An hls playlist for a source, cut on its own keyframes so the player can
 * seek by asking for the segment it wants.
 *
 * A source with no usable index - a live http stream, or mpeg-ts, which
 * carries no index at all - cannot be cut up ahead of time, so it gets a
 * playlist of one segment that runs for as long as the source does. That is
 * enough to get mpeg-ts playing, since the browser takes it through a
 * playlist while refusing it on its own.
 **/
static enum MHD_Result
remux_request_m3u8(struct MHD_Connection *conn) {
  char enc[REMUX_URI_MAX];
  AVFormatContext *ic = 0;
  remux_seg_t *segs = 0;
  double *ks = 0;
  enum MHD_Result res;
  remux_plan_t plan;
  remux_buf_t buf = {0};
  const char *uri;
  double single;
  double target;
  int nseg = 0;
  int nks;

  if(!(uri=remux_arg_uri(conn))) {
    return remux_response_error(conn, MHD_HTTP_BAD_REQUEST,
                                "missing the uri query argument\n");
  }

  if(remux_prepare(conn, uri, &ic, &plan, &res) < 0) {
    return res;
  }

  if(plan.mode != REMUX_MODE_TS) {
    avformat_close_input(&ic);
    return remux_response_error(conn, MHD_HTTP_UNSUPPORTED_MEDIA_TYPE,
                                plan.mode == REMUX_MODE_WEBM
                                ? "this source is vp9 or opus, play it with "
                                  "/remux/stream.webm\n"
                                : plan.reason);
  }

  if(remux_uriencode(uri, enc, sizeof(enc)) < 0) {
    avformat_close_input(&ic);
    return remux_response_error(conn, MHD_HTTP_URI_TOO_LONG, "uri too long\n");
  }

  if(plan.seekable && plan.duration > 0) {
    int sidx = plan.vstream >= 0 ? plan.vstream : plan.astream;

    if(sidx >= 0 &&
       (nks=remux_keyframes(ic, sidx, plan.start / (double)AV_TIME_BASE, &ks))) {
      nseg = remux_segments(ks, nks, plan.duration, &segs);
      free(ks);
    }
  }

  avformat_close_input(&ic);

  /* the whole source as one segment, when there was nothing to cut on */
  single = plan.duration > 0 ? plan.duration : 60.0 * 60 * 24 * 7;

  target = nseg ? 0 : single;
  for(int i=0; i<nseg; i++) {
    if(segs[i].dur > target) {
      target = segs[i].dur;
    }
  }

  remux_buf_addf(&buf,
                 "#EXTM3U\n"
                 "#EXT-X-VERSION:6\n"
                 "#EXT-X-TARGETDURATION:%d\n"
                 "#EXT-X-MEDIA-SEQUENCE:0\n"
                 "#EXT-X-PLAYLIST-TYPE:VOD\n"
                 "#EXT-X-INDEPENDENT-SEGMENTS\n",
                 (int)ceil(target - 0.001));

  if(nseg) {
    for(int i=0; i<nseg; i++) {
      remux_buf_addf(&buf, "#EXTINF:%.6f,\n"
                     "/remux/segment.ts?uri=%s&start=%.6f&dur=%.6f\n",
                     segs[i].dur, enc, segs[i].start, segs[i].dur);
    }
  } else {
    /* Nothing to cut on, so the whole source is one segment. The player
       cannot seek within it, but it does play. */
    remux_buf_addf(&buf, "#EXTINF:%.6f,\n/remux/stream.ts?uri=%s\n",
                   single, enc);
  }

  remux_buf_addf(&buf, "#EXT-X-ENDLIST\n");
  free(segs);

  if(!buf.ptr) {
    return remux_response_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                                "out of memory\n");
  }

  res = remux_response_text(conn, buf.ptr, buf.len,
                            "application/vnd.apple.mpegurl");
  remux_buf_free(&buf);

  return res;
}


/**
 * Stream a window of a source through a muxer. A window of the whole thing is
 * what the stream.* endpoints ask for; a slice of it is one hls segment.
 **/
static enum MHD_Result
remux_stream(struct MHD_Connection *conn, const char* fmt, const char* type,
             double start, double dur) {
  char err[AV_ERROR_MAX_STRING_SIZE] = {0};
  AVFormatContext *ic = 0;
  struct MHD_Response *resp;
  enum MHD_Result res;
  remux_plan_t plan;
  const char *uri;
  remux_t *r;
  int rc;

  if(!(uri=remux_arg_uri(conn))) {
    return remux_response_error(conn, MHD_HTTP_BAD_REQUEST,
                                "missing the uri query argument\n");
  }

  if(remux_prepare(conn, uri, &ic, &plan, &res) < 0) {
    return res;
  }

  if(plan.mode == REMUX_MODE_NONE) {
    avformat_close_input(&ic);
    return remux_response_error(conn, MHD_HTTP_UNSUPPORTED_MEDIA_TYPE,
                                plan.reason);
  }

  if(!(r=calloc(1, sizeof(remux_t)))) {
    avformat_close_input(&ic);
    return remux_response_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                                "out of memory\n");
  }

  r->ifmt = ic;
  r->t0 = plan.start;
  r->base_us = strcmp(fmt, "mpegts") ? 0 : REMUX_TS_BASE;

  if(dur > 0) {
    r->start_us = (int64_t)(start * AV_TIME_BASE);
    r->end_us = r->start_us + (int64_t)(dur * AV_TIME_BASE);
    /* a segment's worth of slack past the end, so a source whose index
       promised a keyframe that is not there cannot run away with the file */
    r->vend_us = r->end_us + (r->end_us - r->start_us) + 10 * AV_TIME_BASE;
  } else {
    r->start_us = INT64_MIN / 2;
    r->end_us = INT64_MAX;
    r->vend_us = INT64_MAX;
    r->vstarted = 1;
    r->vkey_t = INT64_MIN;
  }

  if(!(r->pkt=av_packet_alloc())) {
    remux_free_cb(r);
    return remux_response_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                                "out of memory\n");
  }

  if((rc=remux_open_output(r, &plan, fmt)) < 0) {
    av_strerror(rc, err, sizeof(err));
    remux_free_cb(r);
    return remux_response_error(conn, MHD_HTTP_BAD_GATEWAY, err);
  }

  /* Land on the keyframe that opens the segment. The target is nudged past
     the boundary so that a timestamp that lost a microsecond on its way
     through the playlist does not seek to the keyframe before it. */
  if(dur > 0 && start > 0) {
    av_seek_frame(r->ifmt, -1, r->t0 + r->start_us + 1000, AVSEEK_FLAG_BACKWARD);
  }

  resp = MHD_create_response_from_callback(MHD_SIZE_UNKNOWN, REMUX_BLOCK_SIZE,
                                           remux_read_cb, r, remux_free_cb);
  if(!resp) {
    remux_free_cb(r);
    return remux_response_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                                "out of memory\n");
  }

  MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE, type);
  MHD_add_response_header(resp, MHD_HTTP_HEADER_CACHE_CONTROL, "no-cache");
  MHD_add_response_header(resp, MHD_HTTP_HEADER_ACCESS_CONTROL_ALLOW_ORIGIN,
                          "*");
  res = MHD_queue_response(conn, MHD_HTTP_OK, resp);
  MHD_destroy_response(resp);

  return res;
}


/**
 * One mpeg-ts segment of an hls rendition.
 **/
static enum MHD_Result
remux_request_segment(struct MHD_Connection *conn) {
  const char *start = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND,
                                                  "start");
  const char *dur = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND,
                                                "dur");
  double t = start ? strtod(start, 0) : 0;
  double d = dur ? strtod(dur, 0) : 0;

  if(!(d > 0) || t < 0) {
    return remux_response_error(conn, MHD_HTTP_BAD_REQUEST,
                                "start and dur must describe a segment\n");
  }

  return remux_stream(conn, "mpegts", "video/mp2t", t, d);
}


enum MHD_Result
remux_request(struct MHD_Connection *conn, const char* url) {
  if(!strcmp("/remux/play", url)) {
    return remux_request_play(conn);
  }
  if(!strcmp("/remux/probe.json", url)) {
    return remux_request_probe(conn);
  }
  if(!strcmp("/remux/master.m3u8", url)) {
    return remux_request_m3u8(conn);
  }
  if(!strcmp("/remux/segment.ts", url)) {
    return remux_request_segment(conn);
  }
  if(!strcmp("/remux/stream.ts", url)) {
    return remux_stream(conn, "mpegts", "video/mp2t", 0, 0);
  }
  if(!strcmp("/remux/stream.mp4", url)) {
    return remux_stream(conn, "mp4", "video/mp4", 0, 0);
  }
  if(!strcmp("/remux/stream.webm", url)) {
    return remux_stream(conn, "webm", "video/webm", 0, 0);
  }

  return MHD_NO;
}

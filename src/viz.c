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

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <microhttpd.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#include "viz.h"


/**
 * Video rendition of the audio spectrum.
 **/
#define VIZ_WIDTH   854
#define VIZ_HEIGHT  480
#define VIZ_FPS     30
#define VIZ_DT      (1.0f / VIZ_FPS)

/**
 * Analysis. The spectrum is computed on a mono downmix at a fixed rate,
 * independently of what the source is encoded at.
 **/
#define VIZ_RATE    48000
#define VIZ_FFT     2048
#define VIZ_BANDS   64
#define VIZ_MOTES   84
#define VIZ_HIST    (1 << 16)
#define VIZ_MASK    (VIZ_HIST - 1)

/**
 * Buffer sizes for the muxed output, in bytes.
 **/
#define VIZ_AVIO_SIZE  (16 * 1024)
#define VIZ_BLOCK_SIZE (32 * 1024)

/**
 * Palette, anchored on the UI's accent (#4dd6b8) and shell (#0a0e18) so the
 * animation reads as the same product. Bands ramp teal -> violet with rising
 * frequency, staying inside the cool half of the wheel.
 **/
#define VIZ_HUE_LOW  0.464f
#define VIZ_HUE_HIGH 0.687f

/**
 * Analysis window, in dB, mapped onto 0..1.
 **/
#define VIZ_FLOOR_DB -72.0f
#define VIZ_RANGE_DB 60.0f

#define VIZ_PI 3.14159265358979f

/**
 * libavformat made the avio write callback const in 7.0.
 **/
#if LIBAVFORMAT_VERSION_MAJOR >= 61
#define VIZ_AVIO_BUF const uint8_t*
#else
#define VIZ_AVIO_BUF uint8_t*
#endif


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
 * An rgba color, with components in 0..1.
 **/
typedef struct viz_color {
  float r;
  float g;
  float b;
  float a;
} viz_color_t;


/**
 * An ambient particle drifting up the frame, tied to one spectrum band.
 **/
typedef struct viz_mote {
  float x;
  float y;
  float vx;
  float vy;
  float size;
  float phase;
  float band;
} viz_mote_t;


/**
 * State of one visualization stream.
 **/
typedef struct viz {
  /* analysis */
  float    window[VIZ_FFT];
  float    re[VIZ_FFT];
  float    im[VIZ_FFT];
  int      rev[VIZ_FFT];
  float    twc[VIZ_FFT / 2];
  float    tws[VIZ_FFT / 2];
  int      band_lo[VIZ_BANDS];
  int      band_hi[VIZ_BANDS];
  float    hist[VIZ_HIST];
  int64_t  written;
  int64_t  next_sample;

  /* animation */
  float      band[VIZ_BANDS];
  float      peak[VIZ_BANDS];
  float      energy;
  float      centroid;
  float      bass;
  float      bass_avg;
  float      beat_flash;
  float      since_beat;
  float      time;
  int        silent;
  viz_mote_t motes[VIZ_MOTES];
  uint32_t   rng;
  uint32_t   dither;
  float     *canvas;

  /* input */
  AVFormatContext *ifmt;
  AVCodecContext  *adec;
  AVPacket        *ipkt;
  AVFrame         *iframe;
  int              astream;
  int64_t          first_pts;

  /* output */
  AVFormatContext   *ofmt;
  AVStream          *vst;
  AVStream          *ast;
  AVCodecContext    *venc;
  AVCodecContext    *aenc;
  AVAudioFifo       *fifo;
  SwrContext        *swr_viz;
  SwrContext        *swr_enc;
  struct SwsContext *sws;
  AVFrame           *vframe;
  AVFrame           *rgb;
  AVFrame           *aframe;
  AVPacket          *opkt;
  uint8_t          **viz_buf;
  uint8_t          **enc_buf;
  int                buf_len;
  int                acopy;
  int64_t            frame_nb;
  int64_t            apts;

  /* muxed bytes waiting to be handed to libmicrohttpd */
  uint8_t *out;
  size_t   out_len;
  size_t   out_cap;
  int      eof;
} viz_t;


static inline float
viz_clamp01(float v) {
  return v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
}


static inline float
viz_mix(float a, float b, float t) {
  return a + (b - a) * t;
}


static inline float
viz_max(float a, float b) {
  return a > b ? a : b;
}


static inline float
viz_min(float a, float b) {
  return a < b ? a : b;
}


/**
 * Fraction of the remaining distance to cover in one frame, given a time
 * constant tau.
 **/
static inline float
viz_approach(float tau) {
  return 1.f - expf(-VIZ_DT / tau);
}


static viz_color_t
viz_hsv(float h, float s, float v, float a) {
  float i = floorf((h - floorf(h)) * 6.f);
  float f = (h - floorf(h)) * 6.f - i;
  float p = v * (1.f - s);
  float q = v * (1.f - s * f);
  float t = v * (1.f - s * (1.f - f));
  viz_color_t c = {v, v, v, viz_clamp01(a)};

  switch(((int)i) % 6) {
  case 0: c.r = v; c.g = t; c.b = p; break;
  case 1: c.r = q; c.g = v; c.b = p; break;
  case 2: c.r = p; c.g = v; c.b = t; break;
  case 3: c.r = p; c.g = q; c.b = v; break;
  case 4: c.r = t; c.g = p; c.b = v; break;
  case 5: c.r = v; c.g = p; c.b = q; break;
  }

  c.r = viz_clamp01(c.r);
  c.g = viz_clamp01(c.g);
  c.b = viz_clamp01(c.b);

  return c;
}


static viz_color_t
viz_rgb(float r, float g, float b) {
  viz_color_t c = {viz_clamp01(r / 255.f), viz_clamp01(g / 255.f),
                   viz_clamp01(b / 255.f), 1.f};
  return c;
}


static viz_color_t
viz_lerp(viz_color_t a, viz_color_t b, float t) {
  viz_color_t c;

  c.r = a.r + (b.r - a.r) * t;
  c.g = a.g + (b.g - a.g) * t;
  c.b = a.b + (b.b - a.b) * t;
  c.a = a.a + (b.a - a.a) * t;

  return c;
}


static float
viz_rand(viz_t *v) {
  v->rng ^= v->rng << 13;
  v->rng ^= v->rng >> 17;
  v->rng ^= v->rng << 5;

  return (float)(v->rng & 0xffffff) / (float)0x1000000;
}


/**
 * Precompute the window, the bit-reversal permutation, the twiddles and the
 * bin ranges of each band, and seed the mote field.
 **/
static void
viz_init(viz_t *v) {
  float bin_hz = (float)VIZ_RATE / VIZ_FFT;
  int max_bin = VIZ_FFT / 2 - 1;
  int bits = 0;

  v->rng = 2463534242u;
  v->dither = 12345u;
  v->astream = -1;
  v->first_pts = AV_NOPTS_VALUE;

  for(int i=0; i<VIZ_FFT; i++) {
    v->window[i] = 0.5f * (1.f - cosf(2.f * VIZ_PI * i / (VIZ_FFT - 1)));
  }

  while((1 << bits) < VIZ_FFT) {
    bits++;
  }
  for(int i=0; i<VIZ_FFT; i++) {
    int r = 0;
    for(int b=0; b<bits; b++) {
      if(i & (1 << b)) {
        r |= 1 << (bits - 1 - b);
      }
    }
    v->rev[i] = r;
  }

  for(int i=0; i<VIZ_FFT/2; i++) {
    v->twc[i] = cosf(-2.f * VIZ_PI * i / VIZ_FFT);
    v->tws[i] = sinf(-2.f * VIZ_PI * i / VIZ_FFT);
  }

  /* Log-spaced bands. 30 Hz .. 16 kHz is what matters musically, and keeps
     the lowest band off the DC bin, which is mostly offset noise. */
  for(int b=0; b<VIZ_BANDS; b++) {
    float lo = 30.f * powf(16000.f / 30.f, (float)b / VIZ_BANDS);
    float hi = 30.f * powf(16000.f / 30.f, (float)(b + 1) / VIZ_BANDS);
    int i0 = (int)(lo / bin_hz + 0.5f);
    int i1 = (int)(hi / bin_hz + 0.5f);

    i0 = i0 < 1 ? 1 : (i0 > max_bin ? max_bin : i0);
    i1 = i1 < i0 + 1 ? i0 + 1 : (i1 > max_bin + 1 ? max_bin + 1 : i1);

    v->band_lo[b] = i0;
    v->band_hi[b] = i1;
  }

  /* Motes, seeded across the frame so the field is already populated on the
     first frame instead of drifting in from the bottom edge. */
  for(int i=0; i<VIZ_MOTES; i++) {
    v->motes[i].x     = viz_rand(v);
    v->motes[i].y     = viz_rand(v);
    v->motes[i].vx    = (viz_rand(v) - 0.5f) * 0.012f;
    v->motes[i].vy    = -0.010f - viz_rand(v) * 0.030f;
    v->motes[i].size  = 0.0035f + viz_rand(v) * 0.0075f;
    v->motes[i].phase = viz_rand(v) * 2.f * VIZ_PI;
    v->motes[i].band  = viz_rand(v);
  }
}


/**
 * Fold a stereo sample pair into the mono analysis history. The stereo image
 * is not what the animation reacts to.
 **/
static inline void
viz_push_sample(viz_t *v, float l, float r) {
  v->hist[v->written & VIZ_MASK] = 0.5f * (l + r);
  v->written++;
}


/**
 * Compute the spectrum of the window ending at the current write position,
 * and advance the animation state by one frame.
 **/
static void
viz_analyze(viz_t *v) {
  float up = viz_approach(0.018f);
  float down = viz_approach(0.19f);
  float bin_hz = (float)VIZ_RATE / VIZ_FFT;
  float norm = 4.f / VIZ_FFT; /* 2/N single-sided, /0.5 Hann gain */
  float target[VIZ_BANDS];
  float weighted = 0.f;
  float sum = 0.f;
  float bass = 0.f;

  for(int i=0; i<VIZ_FFT; i++) {
    int64_t idx = v->written - VIZ_FFT + i;
    float s = idx < 0 ? 0.f : v->hist[idx & VIZ_MASK];

    v->re[i] = s * v->window[i];
    v->im[i] = 0.f;
  }

  /* In-place iterative radix-2 FFT. */
  for(int i=0; i<VIZ_FFT; i++) {
    int j = v->rev[i];
    if(j > i) {
      float t;
      t = v->re[i]; v->re[i] = v->re[j]; v->re[j] = t;
      t = v->im[i]; v->im[i] = v->im[j]; v->im[j] = t;
    }
  }
  for(int len=2; len<=VIZ_FFT; len<<=1) {
    int half = len >> 1;
    int step = VIZ_FFT / len;

    for(int i=0; i<VIZ_FFT; i+=len) {
      for(int k=0; k<half; k++) {
        float wr = v->twc[k * step];
        float wi = v->tws[k * step];
        int a = i + k;
        int b = a + half;
        float tr = v->re[b] * wr - v->im[b] * wi;
        float ti = v->re[b] * wi + v->im[b] * wr;

        v->re[b] = v->re[a] - tr;
        v->im[b] = v->im[a] - ti;
        v->re[a] += tr;
        v->im[a] += ti;
      }
    }
  }

  /* Bin -> band. Peak per band rather than mean: it tracks tonal content
     instead of smearing it across the band's noise floor. */
  for(int b=0; b<VIZ_BANDS; b++) {
    float mag = 0.f;
    float f;
    float tilt;
    float db;

    for(int i=v->band_lo[b]; i<v->band_hi[b]; i++) {
      mag = viz_max(mag, sqrtf(v->re[i] * v->re[i] + v->im[i] * v->im[i]));
    }
    db = 20.f * log10f(mag * norm + 1e-9f);

    /* Music falls off with frequency; without a tilt the top third of the
       spectrum never moves. +3 dB/octave above 250 Hz, capped. */
    f = 0.5f * (v->band_lo[b] + v->band_hi[b]) * bin_hz;
    tilt = viz_min(15.f, viz_max(0.f, 3.f * log2f(f / 250.f)));

    target[b] = viz_clamp01((db + tilt - VIZ_FLOOR_DB) / VIZ_RANGE_DB);
  }

  /* Fast attack, slow release: transients read as hits, tails as glow. */
  for(int b=0; b<VIZ_BANDS; b++) {
    float t = target[b];

    v->band[b] += (t - v->band[b]) * (t > v->band[b] ? up : down);
    if(v->band[b] > v->peak[b]) {
      v->peak[b] = v->band[b];
    } else {
      v->peak[b] = viz_max(v->band[b], v->peak[b] - VIZ_DT * 0.42f);
    }

    sum += v->band[b];
    weighted += v->band[b] * (float)b;
  }

  v->energy += (sum / VIZ_BANDS - v->energy) * viz_approach(0.10f);
  if(sum > 0.0001f) {
    v->centroid += ((weighted / sum) / VIZ_BANDS - v->centroid) * viz_approach(0.35f);
  }
  v->silent = sum < 0.0001f;

  /* Beat: bass energy jumping above its own running mean. Cheap, and good
     enough for a background animation -- it does not need to be a beat
     accurate onset detector, just responsive and not twitchy. */
  for(int b=0; b<6; b++) {
    bass += v->band[b];
  }
  v->bass = bass / 6.f;

  v->since_beat += VIZ_DT;
  if(!v->silent && v->bass > v->bass_avg * 1.4f + 0.04f && v->since_beat > 0.14f) {
    v->since_beat = 0.f;
    v->beat_flash = 1.f;
    for(int i=0; i<VIZ_MOTES; i++) {
      v->motes[i].vy -= 0.020f * v->bass;
      v->motes[i].vx += (viz_rand(v) - 0.5f) * 0.030f * v->bass;
    }
  }
  v->bass_avg += (v->bass - v->bass_avg) * viz_approach(0.38f);
  v->beat_flash = viz_max(0.f, v->beat_flash - VIZ_DT * 3.0f);
}


/**
 * Drift the mote field one frame forward.
 **/
static void
viz_motion(viz_t *v) {
  float lift = 1.f + 2.2f * v->energy;

  v->time += VIZ_DT;

  for(int i=0; i<VIZ_MOTES; i++) {
    viz_mote_t *m = &v->motes[i];

    m->x += m->vx * VIZ_DT * lift;
    m->y += m->vy * VIZ_DT * lift;

    /* Ease back toward the resting drift after a beat kick. */
    m->vx *= 1.f - viz_min(1.f, VIZ_DT * 1.6f);
    m->vy += (-0.010f - m->size * 2.2f - m->vy) * viz_min(1.f, VIZ_DT * 1.1f);

    if(m->y < -0.05f) {
      m->y = 1.05f;
      m->x = viz_rand(v);
      m->vx = (viz_rand(v) - 0.5f) * 0.012f;
      m->band = viz_rand(v);
    }
    if(m->x < -0.05f) {
      m->x = 1.05f;
    } else if(m->x > 1.05f) {
      m->x = -0.05f;
    }
  }
}


/**
 * Opaque wash replacing the black clear. Three stops, warmed toward the
 * accent by the current level so loud passages lift the whole room.
 **/
static void
viz_draw_backdrop(viz_t *v) {
  float lift = v->energy * 0.85f + v->beat_flash * 0.25f;
  float hue = viz_mix(VIZ_HUE_LOW, VIZ_HUE_HIGH, viz_clamp01(v->centroid));
  viz_color_t top = viz_rgb(5.f + 10.f * lift, 7.f + 16.f * lift, 15.f + 30.f * lift);
  viz_color_t mid = viz_hsv(hue, 0.72f, 0.06f + 0.16f * lift, 1.f);
  viz_color_t bot = viz_rgb(6.f + 14.f * lift, 12.f + 26.f * lift, 24.f + 44.f * lift);
  const float split = 0.58f;

  for(int y=0; y<VIZ_HEIGHT; y++) {
    float t = (float)y / (VIZ_HEIGHT - 1);
    viz_color_t c = (t < split) ? viz_lerp(top, mid, t / split)
                                : viz_lerp(mid, bot, (t - split) / (1.f - split));
    float *row = v->canvas + (size_t)y * VIZ_WIDTH * 3;

    for(int x=0; x<VIZ_WIDTH; x++) {
      row[x * 3 + 0] = c.r;
      row[x * 3 + 1] = c.g;
      row[x * 3 + 2] = c.b;
    }
  }
}


/**
 * Additive axis-aligned quad with a vertical color ramp, with box coverage
 * along the edges so bars do not jitter as they grow and shrink.
 **/
static void
viz_add_rect(viz_t *v, float x0, float y0, float x1, float y1,
             viz_color_t ctop, viz_color_t cbot) {
  int px0 = (int)floorf(x0);
  int px1 = (int)ceilf(x1);
  int py0 = (int)floorf(y0);
  int py1 = (int)ceilf(y1);

  if(x1 <= x0 || y1 <= y0) {
    return;
  }

  px0 = px0 < 0 ? 0 : px0;
  py0 = py0 < 0 ? 0 : py0;
  px1 = px1 > VIZ_WIDTH ? VIZ_WIDTH : px1;
  py1 = py1 > VIZ_HEIGHT ? VIZ_HEIGHT : py1;

  for(int y=py0; y<py1; y++) {
    float cov_y = viz_min(y + 1.f, y1) - viz_max((float)y, y0);
    float *row = v->canvas + (size_t)y * VIZ_WIDTH * 3;
    viz_color_t c;
    float r, g, b;

    if(cov_y <= 0.f) {
      continue;
    }

    c = viz_lerp(ctop, cbot, viz_clamp01((y + 0.5f - y0) / (y1 - y0)));
    r = c.r * c.a;
    g = c.g * c.a;
    b = c.b * c.a;

    for(int x=px0; x<px1; x++) {
      float cov = (viz_min(x + 1.f, x1) - viz_max((float)x, x0)) * cov_y;

      if(cov <= 0.f) {
        continue;
      }
      row[x * 3 + 0] += r * cov;
      row[x * 3 + 1] += g * cov;
      row[x * 3 + 2] += b * cov;
    }
  }
}


/**
 * One mote: a diamond, bright at the centre and fading to zero alpha at the
 * four tips. Drawn as four gouraud triangles by the SDL version; since both
 * outer vertices of every triangle carry the same color, the interpolated
 * weight of the centre vertex is exactly 1 - (|dx| + |dy|)/s, which collapses
 * the whole thing into a closed form per pixel.
 **/
static void
viz_add_mote(viz_t *v, float cx, float cy, float s,
             viz_color_t core, viz_color_t edge) {
  int px0 = (int)floorf(cx - s);
  int px1 = (int)ceilf(cx + s) + 1;
  int py0 = (int)floorf(cy - s);
  int py1 = (int)ceilf(cy + s) + 1;

  if(s <= 0.f) {
    return;
  }

  px0 = px0 < 0 ? 0 : px0;
  py0 = py0 < 0 ? 0 : py0;
  px1 = px1 > VIZ_WIDTH ? VIZ_WIDTH : px1;
  py1 = py1 > VIZ_HEIGHT ? VIZ_HEIGHT : py1;

  for(int y=py0; y<py1; y++) {
    float dy = fabsf(y + 0.5f - cy) / s;
    float *row = v->canvas + (size_t)y * VIZ_WIDTH * 3;

    if(dy >= 1.f) {
      continue;
    }

    for(int x=px0; x<px1; x++) {
      float dx = fabsf(x + 0.5f - cx) / s;
      float w = 1.f - (dx + dy);
      float a;

      if(w <= 0.f) {
        continue;
      }

      a = core.a * w + edge.a * (1.f - w);
      row[x * 3 + 0] += (core.r * w + edge.r * (1.f - w)) * a;
      row[x * 3 + 1] += (core.g * w + edge.g * (1.f - w)) * a;
      row[x * 3 + 2] += (core.b * w + edge.b * (1.f - w)) * a;
    }
  }
}


/**
 * Ambient motes, each tied to one band so the field shimmers with the mix.
 **/
static void
viz_draw_motes(viz_t *v) {
  for(int i=0; i<VIZ_MOTES; i++) {
    viz_mote_t *m = &v->motes[i];
    int band = (int)(m->band * VIZ_BANDS);
    float twinkle;
    float alpha;
    float hue;
    float s;

    band = band > VIZ_BANDS - 1 ? VIZ_BANDS - 1 : band;
    twinkle = 0.5f + 0.5f * sinf(v->time * 1.7f + m->phase);
    alpha = (0.14f + 0.75f * v->band[band]) * (0.30f + 0.70f * twinkle) * 0.55f;
    if(alpha <= 0.004f) {
      continue;
    }

    s = VIZ_HEIGHT * m->size * (0.75f + 0.9f * v->band[band]);
    hue = viz_mix(VIZ_HUE_LOW, VIZ_HUE_HIGH, m->band);

    viz_add_mote(v, m->x * VIZ_WIDTH, m->y * VIZ_HEIGHT, s,
                 viz_hsv(hue, 0.35f, 1.f, alpha),
                 viz_hsv(hue, 0.80f, 1.f, 0.f));
  }
}


/**
 * Bottom spectrum, grounding the composition against the frame edge.
 **/
static void
viz_draw_bars(viz_t *v) {
  float slot = (float)VIZ_WIDTH / VIZ_BANDS;
  float bw = slot * 0.58f;
  float pad = (slot - bw) * 0.5f;
  float base = VIZ_HEIGHT;

  for(int i=0; i<VIZ_BANDS; i++) {
    float val = v->band[i];
    float x = i * slot + pad;
    float bh = VIZ_HEIGHT * (0.010f + 0.24f * val);
    float ph = VIZ_HEIGHT * (0.010f + 0.24f * v->peak[i]);
    float th = viz_max(1.f, VIZ_HEIGHT * 0.0035f);
    float hue = viz_mix(VIZ_HUE_LOW, VIZ_HUE_HIGH, (float)i / VIZ_BANDS);
    viz_color_t cap = viz_hsv(hue, 0.20f, 1.f, 0.25f + 0.40f * v->peak[i]);

    viz_add_rect(v, x, base - bh, x + bw, base,
                 viz_hsv(hue, 0.85f, 0.95f, 0.02f + 0.12f * val),
                 viz_hsv(hue, 0.45f, 1.00f, 0.34f + 0.46f * val));
    viz_add_rect(v, x, base - ph - th, x + bw, base - ph, cap, cap);
  }
}


/**
 * Quantise the float canvas into the rgb frame handed to swscale. A little
 * noise keeps the very shallow backdrop ramp from banding once it has been
 * reduced to 8 bits and given to the encoder.
 **/
static void
viz_canvas_to_rgb(viz_t *v) {
  for(int y=0; y<VIZ_HEIGHT; y++) {
    const float *src = v->canvas + (size_t)y * VIZ_WIDTH * 3;
    uint8_t *dst = v->rgb->data[0] + (size_t)y * v->rgb->linesize[0];

    for(int i=0; i<VIZ_WIDTH*3; i++) {
      float n;

      v->dither = v->dither * 1664525u + 1013904223u;
      n = (float)((v->dither >> 8) & 0xffff) / 65535.f - 0.5f;
      dst[i] = (uint8_t)(viz_min(255.f, viz_max(0.f, src[i] * 255.f + n)) + 0.5f);
    }
  }
}


/**
 * Append muxed bytes to the queue drained by libmicrohttpd.
 **/
static int
viz_avio_write(void *opaque, VIZ_AVIO_BUF buf, int size) {
  viz_t *v = (viz_t*)opaque;

  if(v->out_len + size > v->out_cap) {
    size_t cap = v->out_len + size + VIZ_BLOCK_SIZE;
    uint8_t *ptr = realloc(v->out, cap);

    if(!ptr) {
      return AVERROR(ENOMEM);
    }
    v->out = ptr;
    v->out_cap = cap;
  }

  memcpy(v->out + v->out_len, buf, size);
  v->out_len += size;

  return size;
}


/**
 * Send a frame to an encoder and mux whatever comes out.
 **/
static int
viz_encode(viz_t *v, AVCodecContext *ctx, AVStream *st, AVFrame *frame) {
  int ret;

  if((ret=avcodec_send_frame(ctx, frame)) < 0) {
    return ret;
  }

  while(1) {
    ret = avcodec_receive_packet(ctx, v->opkt);
    if(ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
      return 0;
    }
    if(ret < 0) {
      return ret;
    }

    av_packet_rescale_ts(v->opkt, ctx->time_base, st->time_base);
    v->opkt->stream_index = st->index;

    ret = av_interleaved_write_frame(v->ofmt, v->opkt);
    av_packet_unref(v->opkt);
    if(ret < 0) {
      return ret;
    }
  }
}


/**
 * Draw and encode one video frame. Frames are emitted the instant their
 * analysis window is complete, so frame n covers exactly t = n/fps of the
 * decoded audio and the two streams stay in step without a clock.
 **/
static int
viz_render_frame(viz_t *v) {
  int ret;

  viz_analyze(v);
  viz_motion(v);

  viz_draw_backdrop(v);
  viz_draw_motes(v);
  viz_draw_bars(v);
  viz_canvas_to_rgb(v);

  if((ret=av_frame_make_writable(v->vframe)) < 0) {
    return ret;
  }

  sws_scale(v->sws, (const uint8_t * const*)v->rgb->data, v->rgb->linesize,
            0, VIZ_HEIGHT, v->vframe->data, v->vframe->linesize);

  v->vframe->pts = v->frame_nb++;
  v->next_sample = av_rescale(v->frame_nb, VIZ_RATE, VIZ_FPS);

  return viz_encode(v, v->venc, v->vst, v->vframe);
}


/**
 * Re-encode audio that is not already aac.
 **/
static int
viz_encode_audio(viz_t *v, AVFrame *frame) {
  int ret;
  int n;

  n = swr_convert(v->swr_enc, v->enc_buf, v->buf_len,
                  frame ? (const uint8_t**)frame->extended_data : 0,
                  frame ? frame->nb_samples : 0);
  while(n > 0) {
    if(av_audio_fifo_write(v->fifo, (void**)v->enc_buf, n) < n) {
      return AVERROR(ENOMEM);
    }
    if((n=swr_convert(v->swr_enc, v->enc_buf, v->buf_len, 0, 0)) < 0) {
      return n;
    }
  }

  while(av_audio_fifo_size(v->fifo) >= v->aenc->frame_size) {
    if((ret=av_frame_make_writable(v->aframe)) < 0) {
      return ret;
    }
    if((ret=av_audio_fifo_read(v->fifo, (void**)v->aframe->data,
                               v->aenc->frame_size)) < 0) {
      return ret;
    }

    v->aframe->pts = v->apts;
    v->apts += v->aenc->frame_size;

    if((ret=viz_encode(v, v->aenc, v->ast, v->aframe)) < 0) {
      return ret;
    }
  }

  return 0;
}


/**
 * Feed decoded audio to the analysis, rendering video frames as their
 * windows complete, and to the audio encoder when re-encoding.
 **/
static int
viz_feed(viz_t *v, AVFrame *frame) {
  int ret;
  int n;

  n = swr_convert(v->swr_viz, v->viz_buf, v->buf_len,
                  frame ? (const uint8_t**)frame->extended_data : 0,
                  frame ? frame->nb_samples : 0);
  while(n > 0) {
    const float *l = (const float*)v->viz_buf[0];
    const float *r = (const float*)v->viz_buf[1];

    for(int i=0; i<n; i++) {
      while(v->next_sample <= v->written) {
        if((ret=viz_render_frame(v)) < 0) {
          return ret;
        }
      }
      viz_push_sample(v, l[i], r[i]);
    }

    if((n=swr_convert(v->swr_viz, v->viz_buf, v->buf_len, 0, 0)) < 0) {
      return n;
    }
  }

  if(!v->acopy) {
    return viz_encode_audio(v, frame);
  }

  return 0;
}


/**
 * Mux an aac packet as-is. Re-encoding it would only lose quality; the video
 * is timed off the same packets, so nothing needs to be aligned by hand.
 **/
static int
viz_copy_audio(viz_t *v, AVPacket *pkt) {
  AVRational tb = v->ifmt->streams[v->astream]->time_base;
  int64_t pts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
  int ret;

  if(pts == AV_NOPTS_VALUE) {
    return 0;
  }
  if(v->first_pts == AV_NOPTS_VALUE) {
    v->first_pts = pts;
  }

  /* Raw aac (as found in mp4) is given adts headers on its way into the
     transport stream, which rewrites the packet. The decoder is holding a
     reference to the same buffer, so hand the muxer a private copy. */
  if(av_packet_ref(v->opkt, pkt) < 0) {
    return 0;
  }
  if(av_packet_make_writable(v->opkt) < 0) {
    av_packet_unref(v->opkt);
    return 0;
  }

  v->opkt->stream_index = v->ast->index;
  v->opkt->pts = av_rescale_q(pts - v->first_pts, tb, v->ast->time_base);
  v->opkt->dts = v->opkt->pts;
  v->opkt->duration = av_rescale_q(pkt->duration, tb, v->ast->time_base);
  v->opkt->pos = -1;

  ret = av_interleaved_write_frame(v->ofmt, v->opkt);
  av_packet_unref(v->opkt);

  return ret;
}


/**
 * Read one packet from the source and push it through the pipeline. Muxed
 * bytes land in the output queue via the avio callback.
 **/
static void
viz_pump(viz_t *v) {
  int ret;

  if((ret=av_read_frame(v->ifmt, v->ipkt)) < 0) {
    /* End of stream: flush decoder, encoders and muxer. */
    avcodec_send_packet(v->adec, 0);
    while(avcodec_receive_frame(v->adec, v->iframe) >= 0) {
      viz_feed(v, v->iframe);
      av_frame_unref(v->iframe);
    }
    viz_feed(v, 0);
    viz_render_frame(v);
    viz_encode(v, v->venc, v->vst, 0);
    if(!v->acopy) {
      viz_encode(v, v->aenc, v->ast, 0);
    }
    av_write_trailer(v->ofmt);
    v->eof = 1;
    return;
  }

  if(v->ipkt->stream_index != v->astream) {
    av_packet_unref(v->ipkt);
    return;
  }

  if(avcodec_send_packet(v->adec, v->ipkt) >= 0) {
    while(avcodec_receive_frame(v->adec, v->iframe) >= 0) {
      if(viz_feed(v, v->iframe) < 0) {
        v->eof = 1;
      }
      av_frame_unref(v->iframe);
    }
  }

  if(v->acopy && viz_copy_audio(v, v->ipkt) < 0) {
    v->eof = 1;
  }

  av_packet_unref(v->ipkt);
}


/**
 * Callback function used to transmit the muxed stream to a http request.
 * libmicrohttpd runs this on the connection's own thread, so pumping the
 * pipeline here is what paces the whole thing.
 **/
static ssize_t
viz_read_cb(void *cls, uint64_t pos, char *buf, size_t max) {
  viz_t *v = (viz_t*)cls;
  size_t n;

  while(!v->out_len && !v->eof) {
    viz_pump(v);
  }

  if(!v->out_len) {
    return MHD_CONTENT_READER_END_OF_STREAM;
  }

  n = v->out_len < max ? v->out_len : max;
  memcpy(buf, v->out, n);
  v->out_len -= n;
  memmove(v->out, v->out + n, v->out_len);

  return n;
}


/**
 * Callback function used to release a visualization that has been
 * transmitted, or whose client went away.
 **/
static void
viz_free_cb(void *cls) {
  viz_t *v = (viz_t*)cls;

  if(v->ofmt) {
    if(v->ofmt->pb) {
      av_freep(&v->ofmt->pb->buffer);
      avio_context_free(&v->ofmt->pb);
    }
    avformat_free_context(v->ofmt);
  }

  avcodec_free_context(&v->venc);
  avcodec_free_context(&v->aenc);
  avcodec_free_context(&v->adec);
  avformat_close_input(&v->ifmt);

  swr_free(&v->swr_viz);
  swr_free(&v->swr_enc);
  sws_freeContext(v->sws);
  if(v->fifo) {
    av_audio_fifo_free(v->fifo);
  }

  av_frame_free(&v->vframe);
  av_frame_free(&v->rgb);
  av_frame_free(&v->aframe);
  av_frame_free(&v->iframe);
  av_packet_free(&v->ipkt);
  av_packet_free(&v->opkt);

  if(v->viz_buf) {
    av_freep(&v->viz_buf[0]);
    av_freep(&v->viz_buf);
  }
  if(v->enc_buf) {
    av_freep(&v->enc_buf[0]);
    av_freep(&v->enc_buf);
  }

  free(v->canvas);
  free(v->out);
  free(v);
}


/**
 * Open the audio source and its decoder. The audio is always decoded, even
 * when it is passed through to the client, since the spectrum is what the
 * video is drawn from.
 **/
static int
viz_open_input(viz_t *v, const char* url) {
  AVDictionary *opts = 0;
  const AVCodec *dec = 0;
  int ret;

  av_dict_set(&opts, "user_agent", "jtplay", 0);
  av_dict_set(&opts, "reconnect", "1", 0);
  av_dict_set(&opts, "reconnect_streamed", "1", 0);

  ret = avformat_open_input(&v->ifmt, url, 0, &opts);
  av_dict_free(&opts);
  if(ret < 0) {
    return ret;
  }

  if((ret=avformat_find_stream_info(v->ifmt, 0)) < 0) {
    return ret;
  }

  ret = av_find_best_stream(v->ifmt, AVMEDIA_TYPE_AUDIO, -1, -1, &dec, 0);
  if(ret < 0) {
    return ret;
  }
  v->astream = ret;

  if(!(v->adec=avcodec_alloc_context3(dec))) {
    return AVERROR(ENOMEM);
  }
  ret = avcodec_parameters_to_context(v->adec,
                                      v->ifmt->streams[v->astream]->codecpar);
  if(ret < 0) {
    return ret;
  }

  if((ret=avcodec_open2(v->adec, dec, 0)) < 0) {
    return ret;
  }

  if(v->adec->ch_layout.nb_channels < 1) {
    return AVERROR(EINVAL);
  }
  if(!av_channel_layout_check(&v->adec->ch_layout)) {
    av_channel_layout_default(&v->adec->ch_layout,
                              v->adec->ch_layout.nb_channels);
  }

  /* Passing aac through keeps the source quality and spares the console an
     encode; anything else has to be converted. */
  v->acopy = v->adec->codec_id == AV_CODEC_ID_AAC;

  return 0;
}


/**
 * Set up the video encoder, the audio stream, and the mpeg-ts muxer that
 * writes into the response queue.
 **/
static int
viz_open_output(viz_t *v) {
  const AVCodec *venc = avcodec_find_encoder_by_name("libx264");
  const AVCodec *aenc = 0;
  AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
  uint8_t *avio_buf;
  int ret;

  if(!venc) {
    return AVERROR_ENCODER_NOT_FOUND;
  }

  ret = avformat_alloc_output_context2(&v->ofmt, 0, "mpegts", 0);
  if(ret < 0) {
    return ret;
  }

  if(!(avio_buf=av_malloc(VIZ_AVIO_SIZE))) {
    return AVERROR(ENOMEM);
  }
  v->ofmt->pb = avio_alloc_context(avio_buf, VIZ_AVIO_SIZE, 1, v, 0,
                                   viz_avio_write, 0);
  if(!v->ofmt->pb) {
    av_free(avio_buf);
    return AVERROR(ENOMEM);
  }
  v->ofmt->flags |= AVFMT_FLAG_CUSTOM_IO | AVFMT_FLAG_FLUSH_PACKETS;

  /* video */
  if(!(v->vst=avformat_new_stream(v->ofmt, 0))) {
    return AVERROR(ENOMEM);
  }
  if(!(v->venc=avcodec_alloc_context3(venc))) {
    return AVERROR(ENOMEM);
  }

  v->venc->width = VIZ_WIDTH;
  v->venc->height = VIZ_HEIGHT;
  v->venc->pix_fmt = AV_PIX_FMT_YUV420P;
  v->venc->time_base = (AVRational){1, VIZ_FPS};
  v->venc->framerate = (AVRational){VIZ_FPS, 1};
  v->venc->gop_size = VIZ_FPS * 2;
  v->venc->max_b_frames = 0;
  v->venc->colorspace = AVCOL_SPC_BT709;
  v->venc->color_primaries = AVCOL_PRI_BT709;
  v->venc->color_trc = AVCOL_TRC_BT709;
  v->venc->color_range = AVCOL_RANGE_MPEG;
  v->venc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

  /* The client is watching this live, so latency beats compression. */
  av_opt_set(v->venc->priv_data, "preset", "veryfast", 0);
  av_opt_set(v->venc->priv_data, "tune", "zerolatency", 0);
  av_opt_set(v->venc->priv_data, "crf", "23", 0);

  if((ret=avcodec_open2(v->venc, venc, 0)) < 0) {
    return ret;
  }
  if((ret=avcodec_parameters_from_context(v->vst->codecpar, v->venc)) < 0) {
    return ret;
  }
  v->vst->time_base = v->venc->time_base;
  v->vst->avg_frame_rate = v->venc->framerate;

  /* audio */
  if(!(v->ast=avformat_new_stream(v->ofmt, 0))) {
    return AVERROR(ENOMEM);
  }

  if(v->acopy) {
    ret = avcodec_parameters_copy(v->ast->codecpar,
                                  v->ifmt->streams[v->astream]->codecpar);
    if(ret < 0) {
      return ret;
    }
    v->ast->codecpar->codec_tag = 0;
    v->ast->time_base = v->ifmt->streams[v->astream]->time_base;

  } else {
    if(!(aenc=avcodec_find_encoder(AV_CODEC_ID_AAC))) {
      return AVERROR_ENCODER_NOT_FOUND;
    }
    if(!(v->aenc=avcodec_alloc_context3(aenc))) {
      return AVERROR(ENOMEM);
    }

    av_channel_layout_copy(&v->aenc->ch_layout, &stereo);
    v->aenc->sample_fmt = AV_SAMPLE_FMT_FLTP;
    v->aenc->sample_rate = VIZ_RATE;
    v->aenc->bit_rate = 192000;
    v->aenc->time_base = (AVRational){1, VIZ_RATE};
    v->aenc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if((ret=avcodec_open2(v->aenc, aenc, 0)) < 0) {
      return ret;
    }
    if((ret=avcodec_parameters_from_context(v->ast->codecpar, v->aenc)) < 0) {
      return ret;
    }
    v->ast->time_base = v->aenc->time_base;
  }

  return avformat_write_header(v->ofmt, 0);
}


/**
 * Set up resampling, frame buffers and the rgb to yuv converter.
 **/
static int
viz_open_filters(viz_t *v) {
  AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
  int ret;

  /* One resampler feeds the analysis at a fixed rate, ... */
  ret = swr_alloc_set_opts2(&v->swr_viz, &stereo, AV_SAMPLE_FMT_FLTP, VIZ_RATE,
                            &v->adec->ch_layout, v->adec->sample_fmt,
                            v->adec->sample_rate, 0, 0);
  if(ret < 0 || (ret=swr_init(v->swr_viz)) < 0) {
    return ret;
  }

  v->buf_len = 1 << 15;
  ret = av_samples_alloc_array_and_samples(&v->viz_buf, 0, 2, v->buf_len,
                                           AV_SAMPLE_FMT_FLTP, 0);
  if(ret < 0) {
    return ret;
  }

  /* ... the other the encoder, when there is one. */
  if(!v->acopy) {
    ret = swr_alloc_set_opts2(&v->swr_enc, &v->aenc->ch_layout,
                              v->aenc->sample_fmt, v->aenc->sample_rate,
                              &v->adec->ch_layout, v->adec->sample_fmt,
                              v->adec->sample_rate, 0, 0);
    if(ret < 0 || (ret=swr_init(v->swr_enc)) < 0) {
      return ret;
    }

    ret = av_samples_alloc_array_and_samples(&v->enc_buf, 0,
                                             v->aenc->ch_layout.nb_channels,
                                             v->buf_len, v->aenc->sample_fmt, 0);
    if(ret < 0) {
      return ret;
    }

    v->fifo = av_audio_fifo_alloc(v->aenc->sample_fmt,
                                  v->aenc->ch_layout.nb_channels,
                                  v->aenc->frame_size * 8);
    if(!v->fifo) {
      return AVERROR(ENOMEM);
    }

    if(!(v->aframe=av_frame_alloc())) {
      return AVERROR(ENOMEM);
    }
    v->aframe->format = v->aenc->sample_fmt;
    v->aframe->sample_rate = v->aenc->sample_rate;
    v->aframe->nb_samples = v->aenc->frame_size;
    av_channel_layout_copy(&v->aframe->ch_layout, &v->aenc->ch_layout);
    if((ret=av_frame_get_buffer(v->aframe, 0)) < 0) {
      return ret;
    }
  }

  if(!(v->vframe=av_frame_alloc()) || !(v->rgb=av_frame_alloc())) {
    return AVERROR(ENOMEM);
  }

  v->vframe->format = AV_PIX_FMT_YUV420P;
  v->vframe->width = VIZ_WIDTH;
  v->vframe->height = VIZ_HEIGHT;
  if((ret=av_frame_get_buffer(v->vframe, 0)) < 0) {
    return ret;
  }
  v->vframe->colorspace = AVCOL_SPC_BT709;
  v->vframe->color_range = AVCOL_RANGE_MPEG;

  v->rgb->format = AV_PIX_FMT_RGB24;
  v->rgb->width = VIZ_WIDTH;
  v->rgb->height = VIZ_HEIGHT;
  if((ret=av_frame_get_buffer(v->rgb, 0)) < 0) {
    return ret;
  }

  v->sws = sws_getContext(VIZ_WIDTH, VIZ_HEIGHT, AV_PIX_FMT_RGB24,
                          VIZ_WIDTH, VIZ_HEIGHT, AV_PIX_FMT_YUV420P,
                          SWS_POINT, 0, 0, 0);
  if(!v->sws) {
    return AVERROR(ENOMEM);
  }

  /* Tell swscale the destination is bt.709, otherwise it assumes bt.601 and
     the teal drifts once a player decodes it as 709. */
  sws_setColorspaceDetails(v->sws, sws_getCoefficients(SWS_CS_ITU601), 1,
                           sws_getCoefficients(SWS_CS_ITU709), 0, 0, 1 << 16,
                           1 << 16);

  if(!(v->ipkt=av_packet_alloc()) || !(v->opkt=av_packet_alloc()) ||
     !(v->iframe=av_frame_alloc())) {
    return AVERROR(ENOMEM);
  }

  if(!(v->canvas=calloc((size_t)VIZ_WIDTH * VIZ_HEIGHT * 3, sizeof(float)))) {
    return AVERROR(ENOMEM);
  }

  return 0;
}


/**
 * Respond with an error page.
 **/
static enum MHD_Result
viz_response_error(struct MHD_Connection *conn, unsigned int status,
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
    MHD_add_response_header(resp, MHD_HTTP_HEADER_ACCESS_CONTROL_ALLOW_ORIGIN, "*");
    ret = MHD_queue_response(conn, status, resp);
    MHD_destroy_response(resp);
  }

  return ret;
}


static int
viz_uriencode(const char* in, char* out, size_t outsize) {
  static const char hex[] = "0123456789ABCDEF";
  size_t pos = 0;
  uint8_t c;

  if(!in || !out || !outsize) {
    return -1;
  }

  while(*in) {
    c = (uint8_t)*in++;

    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
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


static enum MHD_Result
viz_request_m3u8(struct MHD_Connection *conn, const char* url) {
  enum MHD_Result ret = MHD_NO;
  struct MHD_Response *resp;
  char uri[PATH_MAX*3] = {0};
  char buf[PATH_MAX*3+512];
  AVFormatContext *ic = 0;
  double duration = 0;
  const char* s;
  int n;

  if(!(s=MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "uri"))) {
    if((resp=MHD_create_response_from_buffer(0, "", MHD_RESPMEM_PERSISTENT))) {
      ret = MHD_queue_response(conn, MHD_HTTP_BAD_REQUEST, resp);
      MHD_destroy_response(resp);
    }
    return ret;
  }

  if(avformat_open_input(&ic, s, 0, 0) < 0) {
    if((resp=MHD_create_response_from_buffer(0, "", MHD_RESPMEM_PERSISTENT))) {
      ret = MHD_queue_response(conn, MHD_HTTP_BAD_REQUEST, resp);
      MHD_destroy_response(resp);
    }
    return ret;
  }

  if(avformat_find_stream_info(ic, 0) >= 0 && ic->duration != AV_NOPTS_VALUE) {
    duration = (double)ic->duration / AV_TIME_BASE;
  }
  avformat_close_input(&ic);

  viz_uriencode(s, uri, sizeof(uri));
  n = snprintf(buf, sizeof(buf),
	       "#EXTM3U\n"
	       "#EXT-X-VERSION:6\n"
	       "#EXT-X-TARGETDURATION:%d\n"
	       "#EXT-X-MEDIA-SEQUENCE:0\n"
	       "#EXT-X-PLAYLIST-TYPE:VOD\n"
	       "#EXT-X-INDEPENDENT-SEGMENTS\n"
	       "#EXTINF:%f,\n"
	       "/viz/stream.ts?uri=%s\n"
	       "#EXT-X-ENDLIST\n",
	       (int)duration, duration  > 0? duration : 60*60*24*7, uri);

  if(n >= (int)sizeof(buf)) {
    n = sizeof(buf) - 1;
  }

  if((resp=MHD_create_response_from_buffer(n, buf, MHD_RESPMEM_MUST_COPY))) {
    MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE,
			    "application/vnd.apple.mpegurl");
    MHD_add_response_header(resp, MHD_HTTP_HEADER_ACCESS_CONTROL_ALLOW_ORIGIN,
			    "*");
    ret = MHD_queue_response(conn, MHD_HTTP_OK, resp);
    MHD_destroy_response(resp);
  }

  return ret;
}


static enum MHD_Result
viz_request_ts(struct MHD_Connection *conn, const char* url) {
  char err[AV_ERROR_MAX_STRING_SIZE] = {0};
  enum MHD_Result ret = MHD_NO;
  struct MHD_Response *resp;
  const char* media_url;
  viz_t *v;
  int rc;

  media_url = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "uri");
  if(!media_url || !media_url[0]) {
    return viz_response_error(conn, MHD_HTTP_BAD_REQUEST,
                              "missing the url query argument\n");
  }

  avformat_network_init();
  av_log_set_level(AV_LOG_ERROR);

  if(!(v=calloc(1, sizeof(viz_t)))) {
    return viz_response_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                              "out of memory\n");
  }

  viz_init(v);

  if((rc=viz_open_input(v, media_url)) < 0 ||
     (rc=viz_open_output(v)) < 0 ||
     (rc=viz_open_filters(v)) < 0) {
    av_strerror(rc, err, sizeof(err));
    viz_free_cb(v);
    return viz_response_error(conn, MHD_HTTP_BAD_GATEWAY, err);
  }

  resp = MHD_create_response_from_callback(MHD_SIZE_UNKNOWN, VIZ_BLOCK_SIZE,
                                           viz_read_cb, v, viz_free_cb);
  if(!resp) {
    viz_free_cb(v);
    return viz_response_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                              "out of memory\n");
  }

  MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE, "video/mp2t");
  MHD_add_response_header(resp, MHD_HTTP_HEADER_CACHE_CONTROL, "no-cache");
  MHD_add_response_header(resp, MHD_HTTP_HEADER_ACCESS_CONTROL_ALLOW_ORIGIN, "*");
  ret = MHD_queue_response(conn, MHD_HTTP_OK, resp);
  MHD_destroy_response(resp);

  return ret;
}


enum MHD_Result
viz_request(struct MHD_Connection *conn, const char* url) {
  if(!strcmp("/viz/master.m3u8", url)) {
    return viz_request_m3u8(conn, url);
  }

  if(!strcmp("/viz/stream.ts", url)) {
    return viz_request_ts(conn, url);
  }

  return MHD_NO;
}

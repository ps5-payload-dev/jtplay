// SPDX-License-Identifier: GPL-3.0-or-later
//
// See visualizer.h for how the audio tap and the clock alignment work.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "visualizer.h"

#if !SDL_VERSION_ATLEAST(2, 0, 18)
#error "visualizer.cpp needs SDL 2.0.18+ for SDL_RenderGeometry"
#endif

namespace {

constexpr float kPi = 3.14159265358979f;

// Palette. Anchored on the UI's accent (#4dd6b8) and shell (#0a0e18) so the
// animation reads as the same product rather than a bolted-on winamp skin.
// Bands ramp teal -> violet with rising frequency, staying inside the cool
// half of the wheel the rest of the interface lives in.
constexpr float kHueLow = 0.464f;   // #4dd6b8
constexpr float kHueHigh = 0.687f;  // #7c6cf0

// Height of the offscreen target, and the one knob worth touching if this
// costs too much on your setup. Everything except the fixed overhead (the
// FFT and the upscale blit, together about 1.5 ms) scales with its square:
// 216 measures ~6 ms/frame, 180 about ~4.7 ms, 270 about ~8 ms. The
// animation is all soft gradients, so it survives being drawn small and
// scaled up; the software rasteriser does not survive being asked to blend
// two megapixels a frame.
constexpr int kTargetHeight = 216;

// Analysis window, in dB, mapped onto 0..1.
constexpr float kFloorDb = -72.f;
constexpr float kRangeDb = 60.f;

inline float Clamp01(float v)
{
	return v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
}

inline float Mix(float a, float b, float t)
{
	return a + (b - a) * t;
}

// Frame-rate independent approach: fraction of the remaining distance to
// cover in 'dt' seconds given a time constant 'tau'.
inline float Approach(float dt, float tau)
{
	return 1.f - std::exp(-dt / tau);
}

SDL_Color Hsv(float h, float s, float v, float a)
{
	h -= std::floor(h);
	const float i = std::floor(h * 6.f);
	const float f = h * 6.f - i;
	const float p = v * (1.f - s);
	const float q = v * (1.f - s * f);
	const float t = v * (1.f - s * (1.f - f));
	float r = v, g = v, b = v;
	switch ((int)i % 6)
	{
	case 0: r = v; g = t; b = p; break;
	case 1: r = q; g = v; b = p; break;
	case 2: r = p; g = v; b = t; break;
	case 3: r = p; g = q; b = v; break;
	case 4: r = t; g = p; b = v; break;
	case 5: r = v; g = p; b = q; break;
	}
	SDL_Color c;
	c.r = (Uint8)(Clamp01(r) * 255.f + 0.5f);
	c.g = (Uint8)(Clamp01(g) * 255.f + 0.5f);
	c.b = (Uint8)(Clamp01(b) * 255.f + 0.5f);
	c.a = (Uint8)(Clamp01(a) * 255.f + 0.5f);
	return c;
}

inline SDL_Color Rgb(int r, int g, int b, int a = 255)
{
	SDL_Color c;
	c.r = (Uint8)std::min(255, std::max(0, r));
	c.g = (Uint8)std::min(255, std::max(0, g));
	c.b = (Uint8)std::min(255, std::max(0, b));
	c.a = (Uint8)std::min(255, std::max(0, a));
	return c;
}

inline SDL_Color LerpC(SDL_Color a, SDL_Color b, float t)
{
	SDL_Color c;
	c.r = (Uint8)(a.r + (b.r - a.r) * t);
	c.g = (Uint8)(a.g + (b.g - a.g) * t);
	c.b = (Uint8)(a.b + (b.b - a.b) * t);
	c.a = 255;
	return c;
}

inline SDL_FPoint Pt(float x, float y)
{
	SDL_FPoint p;
	p.x = x;
	p.y = y;
	return p;
}

} // namespace

// ---------------------------------------------------------------------------
// Geometry batching
// ---------------------------------------------------------------------------

void Visualizer::Batch::Reset()
{
	verts.clear();
	indices.clear();
}

void Visualizer::Batch::Tri(SDL_FPoint a, SDL_FPoint b, SDL_FPoint c,
                            SDL_Color ca, SDL_Color cb, SDL_Color cc)
{
	const int base = (int)verts.size();
	SDL_Vertex v = {};
	v.position = a; v.color = ca; verts.push_back(v);
	v.position = b; v.color = cb; verts.push_back(v);
	v.position = c; v.color = cc; verts.push_back(v);
	indices.push_back(base + 0);
	indices.push_back(base + 1);
	indices.push_back(base + 2);
}

void Visualizer::Batch::Quad(const SDL_FPoint p[4], const SDL_Color c[4])
{
	const int base = (int)verts.size();
	SDL_Vertex v = {};
	for (int i = 0; i < 4; ++i)
	{
		v.position = p[i];
		v.color = c[i];
		verts.push_back(v);
	}
	indices.push_back(base + 0);
	indices.push_back(base + 1);
	indices.push_back(base + 2);
	indices.push_back(base + 0);
	indices.push_back(base + 2);
	indices.push_back(base + 3);
}

void Visualizer::Batch::Flush(SDL_Renderer* renderer)
{
	if (indices.empty())
		return;
	SDL_RenderGeometry(renderer, nullptr, verts.data(), (int)verts.size(),
	                   indices.data(), (int)indices.size());
	Reset();
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

Visualizer::Visualizer()
{
	history_.assign(kHistory, 0.f);

	// Hann window.
	window_.resize(kFFT);
	for (int i = 0; i < kFFT; ++i)
		window_[i] = 0.5f * (1.f - std::cos(2.f * kPi * i / (kFFT - 1)));

	re_.resize(kFFT);
	im_.resize(kFFT);

	// Bit-reversal permutation.
	rev_.resize(kFFT);
	int bits = 0;
	while ((1 << bits) < kFFT)
		++bits;
	for (int i = 0; i < kFFT; ++i)
	{
		int r = 0;
		for (int b = 0; b < bits; ++b)
			if (i & (1 << b))
				r |= 1 << (bits - 1 - b);
		rev_[i] = r;
	}

	// Twiddles, indexed by half-block size.
	tw_cos_.resize(kFFT / 2);
	tw_sin_.resize(kFFT / 2);
	for (int i = 0; i < kFFT / 2; ++i)
	{
		tw_cos_[i] = std::cos(-2.f * kPi * i / kFFT);
		tw_sin_[i] = std::sin(-2.f * kPi * i / kFFT);
	}

	// Log-spaced bands. 30 Hz .. 16 kHz is what matters musically and keeps
	// the lowest band off the DC bin, which is mostly offset noise.
	band_lo_.resize(kVizBands);
	band_hi_.resize(kVizBands);
	const float bin_hz = (float)kVizRate / kFFT;
	const int max_bin = kFFT / 2 - 1;
	for (int b = 0; b < kVizBands; ++b)
	{
		const float lo = 30.f * std::pow(16000.f / 30.f, (float)b / kVizBands);
		const float hi = 30.f * std::pow(16000.f / 30.f, (float)(b + 1) / kVizBands);
		int i0 = (int)(lo / bin_hz + 0.5f);
		int i1 = (int)(hi / bin_hz + 0.5f);
		i0 = std::min(std::max(i0, 1), max_bin);
		i1 = std::min(std::max(i1, i0 + 1), max_bin + 1);
		band_lo_[b] = i0;
		band_hi_[b] = i1;
	}

	// Motes, seeded across the screen so the field is already populated on
	// the first frame instead of drifting in from the bottom edge.
	motes_.resize(84);
	for (Mote& m : motes_)
	{
		m.x = Rand();
		m.y = Rand();
		m.vx = (Rand() - 0.5f) * 0.012f;
		m.vy = -0.010f - Rand() * 0.030f;
		m.size = 0.0035f + Rand() * 0.0075f;
		m.phase = Rand() * 2.f * kPi;
		m.band = Rand();
	}
}

float Visualizer::Rand()
{
	rng_ ^= rng_ << 13;
	rng_ ^= rng_ >> 17;
	rng_ ^= rng_ << 5;
	return (float)(rng_ & 0xffffff) / (float)0x1000000;
}

// ---------------------------------------------------------------------------
// Audio tap
// ---------------------------------------------------------------------------

void Visualizer::Push(const int16_t* interleaved, int frames, int64_t pts_end_us)
{
	if (!interleaved || frames <= 0)
		return;

	std::lock_guard<std::mutex> lock(mutex_);
	for (int i = 0; i < frames; ++i)
	{
		// Downmix; the analysis is mono and the stereo image is not what the
		// animation reacts to.
		const float l = interleaved[i * kVizChannels + 0] / 32768.f;
		const float r = interleaved[i * kVizChannels + 1] / 32768.f;
		history_[(size_t)((written_ + i) % kHistory)] = 0.5f * (l + r);
	}
	written_ += frames;
	pts_end_us_ = pts_end_us;
}

void Visualizer::Reset()
{
	std::lock_guard<std::mutex> lock(mutex_);
	std::fill(history_.begin(), history_.end(), 0.f);
	written_ = 0;
	pts_end_us_ = INT64_MIN;
}

bool Visualizer::FetchWindow(int64_t clock_us, float* out)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (written_ < kFFT)
		return false;

	// How far behind the newest decoded sample the speakers currently are.
	// MasterClock() derives this from the audio device's queue depth, so the
	// window lands on what is audible rather than what was last decoded.
	// Without timestamps there is nothing to align to, so use the newest
	// samples and accept that the animation leads the music.
	int64_t behind = 0;
	if (clock_us != INT64_MIN && pts_end_us_ != INT64_MIN)
		behind = (pts_end_us_ - clock_us) * kVizRate / 1000000;
	behind = std::max<int64_t>(0, behind);

	// The audible samples are older than anything still held here, because
	// the decoder ran further ahead of playback than the ring buffer covers
	// and lapped them. Clamping to the oldest samples available would pin
	// the analysis to one fixed window, which looks like the animation has
	// hung; report no data instead and let the idle motion carry it until
	// playback catches up.
	if (behind > kHistory - kFFT)
		return false;

	const int64_t end = written_ - behind;
	if (end < kFFT)
		return false;

	const int64_t start = end - kFFT;
	for (int i = 0; i < kFFT; ++i)
		out[i] = history_[(size_t)((start + i) % kHistory)];
	return true;
}

// ---------------------------------------------------------------------------
// Analysis
// ---------------------------------------------------------------------------

void Visualizer::Analyze(int64_t clock_us, float dt, bool paused)
{
	float target[kVizBands];
	const bool have = !paused && FetchWindow(clock_us, re_.data());
	silent_ = !have;

	if (have)
	{
		for (int i = 0; i < kFFT; ++i)
		{
			re_[i] *= window_[i];
			im_[i] = 0.f;
		}

		// In-place iterative radix-2 FFT.
		for (int i = 0; i < kFFT; ++i)
		{
			const int j = rev_[i];
			if (j > i)
			{
				std::swap(re_[i], re_[j]);
				std::swap(im_[i], im_[j]);
			}
		}
		for (int len = 2; len <= kFFT; len <<= 1)
		{
			const int half = len >> 1;
			const int step = kFFT / len;
			for (int i = 0; i < kFFT; i += len)
			{
				for (int k = 0; k < half; ++k)
				{
					const float wr = tw_cos_[k * step];
					const float wi = tw_sin_[k * step];
					const int a = i + k;
					const int b = a + half;
					const float tr = re_[b] * wr - im_[b] * wi;
					const float ti = re_[b] * wi + im_[b] * wr;
					re_[b] = re_[a] - tr;
					im_[b] = im_[a] - ti;
					re_[a] += tr;
					im_[a] += ti;
				}
			}
		}

		// Bin -> band. Peak per band rather than mean: it tracks tonal
		// content instead of smearing it across the band's noise floor.
		const float bin_hz = (float)kVizRate / kFFT;
		const float norm = 4.f / kFFT; // 2/N single-sided, /0.5 Hann gain
		for (int b = 0; b < kVizBands; ++b)
		{
			float mag = 0.f;
			for (int i = band_lo_[b]; i < band_hi_[b]; ++i)
			{
				const float m = std::sqrt(re_[i] * re_[i] + im_[i] * im_[i]);
				mag = std::max(mag, m);
			}
			const float db = 20.f * std::log10(mag * norm + 1e-9f);

			// Music falls off with frequency; without a tilt the top third of
			// the spectrum never moves. +3 dB/octave above 250 Hz, capped.
			const float f = 0.5f * (band_lo_[b] + band_hi_[b]) * bin_hz;
			const float tilt = std::min(15.f, std::max(0.f, 3.f * std::log2(f / 250.f)));

			target[b] = Clamp01((db + tilt - kFloorDb) / kRangeDb);
		}
	}
	else
	{
		// Idle: a slow travelling swell, so a paused or not-yet-started track
		// still breathes instead of snapping to a dead screen.
		for (int b = 0; b < kVizBands; ++b)
		{
			const float t = (float)b / kVizBands;
			const float s = 0.5f + 0.5f * std::sin(time_ * 0.9f - t * 4.2f);
			target[b] = (paused ? 0.05f : 0.07f) * s * (1.f - 0.55f * t);
		}
	}

	// Fast attack, slow release: transients read as hits, tails as glow.
	const float up = Approach(dt, 0.018f);
	const float down = Approach(dt, 0.19f);
	float sum = 0.f, weighted = 0.f;
	for (int b = 0; b < kVizBands; ++b)
	{
		const float t = target[b];
		band_[b] += (t - band_[b]) * (t > band_[b] ? up : down);

		if (band_[b] > peak_[b])
			peak_[b] = band_[b];
		else
			peak_[b] = std::max(band_[b], peak_[b] - dt * 0.42f);

		sum += band_[b];
		weighted += band_[b] * (float)b;
	}

	energy_ += (sum / kVizBands - energy_) * Approach(dt, 0.10f);
	if (sum > 0.0001f)
		centroid_ += ((weighted / sum) / kVizBands - centroid_) * Approach(dt, 0.35f);

	// Beat: bass energy jumping above its own running mean. Cheap, and good
	// enough for a background animation -- it does not need to be a
	// beat-accurate onset detector, just responsive and not twitchy.
	float bass = 0.f;
	for (int b = 0; b < 6; ++b)
		bass += band_[b];
	bass_ = bass / 6.f;

	since_beat_ += dt;
	if (!silent_ && bass_ > bass_avg_ * 1.4f + 0.04f && since_beat_ > 0.14f)
	{
		since_beat_ = 0.f;
		beat_flash_ = 1.f;
		for (Mote& m : motes_)
		{
			m.vy -= 0.020f * bass_;
			m.vx += (Rand() - 0.5f) * 0.030f * bass_;
		}
	}
	bass_avg_ += (bass_ - bass_avg_) * Approach(dt, 0.38f);
	beat_flash_ = std::max(0.f, beat_flash_ - dt * 3.0f);
}

void Visualizer::UpdateMotion(float dt)
{
	time_ += dt;

	const float lift = 1.f + 2.2f * energy_;
	for (Mote& m : motes_)
	{
		m.x += m.vx * dt * lift;
		m.y += m.vy * dt * lift;
		// Ease back toward the resting drift after a beat kick.
		m.vx *= 1.f - std::min(1.f, dt * 1.6f);
		m.vy += (-0.010f - m.size * 2.2f - m.vy) * std::min(1.f, dt * 1.1f);

		if (m.y < -0.05f)
		{
			m.y = 1.05f;
			m.x = Rand();
			m.vx = (Rand() - 0.5f) * 0.012f;
			m.band = Rand();
		}
		if (m.x < -0.05f)
			m.x = 1.05f;
		else if (m.x > 1.05f)
			m.x = -0.05f;
	}
}

// ---------------------------------------------------------------------------
// Layers
// ---------------------------------------------------------------------------

// Opaque wash replacing the black clear. Three stops, warmed toward the
// accent by the current level so loud passages lift the whole room.
//
// Drawn as solid strips rather than one interpolated quad. SDL's software
// renderer shades geometry per pixel in C but fills a solid rect with a
// memset, which measured 25x faster for the same gradient. Adjacent strips
// differ by about one 255th, so no banding survives the upscale.
void Visualizer::DrawBackdrop(SDL_Renderer* renderer, int w, int h)
{
	const float lift = energy_ * 0.85f + beat_flash_ * 0.25f;
	const float hue = Mix(kHueLow, kHueHigh, Clamp01(centroid_));

	const SDL_Color top = Rgb(5 + (int)(10 * lift), 7 + (int)(16 * lift), 15 + (int)(30 * lift));
	const SDL_Color mid = Hsv(hue, 0.72f, 0.06f + 0.16f * lift, 1.f);
	const SDL_Color bot = Rgb(6 + (int)(14 * lift), 12 + (int)(26 * lift), 24 + (int)(44 * lift));

	constexpr int kStrips = 54;
	constexpr float kSplit = 0.58f;
	SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
	for (int i = 0; i < kStrips; ++i)
	{
		const float t = (float)i / (kStrips - 1);
		const SDL_Color c = (t < kSplit) ? LerpC(top, mid, t / kSplit)
		                                 : LerpC(mid, bot, (t - kSplit) / (1.f - kSplit));
		const int y0 = (int)((float)i * h / kStrips);
		const int y1 = (int)((float)(i + 1) * h / kStrips);
		SDL_Rect rc = { 0, y0, w, std::max(1, y1 - y0) };
		SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, 255);
		SDL_RenderFillRect(renderer, &rc);
	}
}


// Bottom spectrum, grounding the composition against the screen edge.
void Visualizer::DrawBars(Batch& b, int w, int h)
{
	const float slot = (float)w / kVizBands;
	const float bw = slot * 0.58f;
	const float pad = (slot - bw) * 0.5f;
	const float base = (float)h;

	for (int i = 0; i < kVizBands; ++i)
	{
		const float v = band_[i];
		const float bh = h * (0.010f + 0.24f * v);
		const float x = i * slot + pad;
		const float hue = Mix(kHueLow, kHueHigh, (float)i / kVizBands);

		const SDL_Color foot = Hsv(hue, 0.45f, 1.f, 0.34f + 0.46f * v);
		const SDL_Color tip = Hsv(hue, 0.85f, 0.95f, 0.02f + 0.12f * v);

		SDL_FPoint p[4] = {
			Pt(x, base - bh), Pt(x + bw, base - bh),
			Pt(x + bw, base), Pt(x, base),
		};
		SDL_Color c[4] = { tip, tip, foot, foot };
		b.Quad(p, c);

		const float ph = h * (0.010f + 0.24f * peak_[i]);
		const float th = std::max(1.f, h * 0.0035f);
		const SDL_Color cap = Hsv(hue, 0.20f, 1.f, 0.25f + 0.40f * peak_[i]);
		SDL_FPoint q[4] = {
			Pt(x, base - ph - th), Pt(x + bw, base - ph - th),
			Pt(x + bw, base - ph), Pt(x, base - ph),
		};
		SDL_Color cc[4] = { cap, cap, cap, cap };
		b.Quad(q, cc);
	}
}

// Ambient motes, each tied to one band so the field shimmers with the mix.
void Visualizer::DrawMotes(Batch& b, int w, int h)
{
	for (const Mote& m : motes_)
	{
		const int band = std::min(kVizBands - 1, (int)(m.band * kVizBands));
		const float twinkle = 0.5f + 0.5f * std::sin(time_ * 1.7f + m.phase);
		const float amp = 0.14f + 0.75f * band_[band];
		const float alpha = amp * (0.30f + 0.70f * twinkle) * 0.55f;
		if (alpha <= 0.004f)
			continue;

		const float s = h * m.size * (0.75f + 0.9f * band_[band]);
		const float x = m.x * w, y = m.y * h;
		const float hue = Mix(kHueLow, kHueHigh, m.band);
		const SDL_Color core = Hsv(hue, 0.35f, 1.f, alpha);
		const SDL_Color edge = Hsv(hue, 0.80f, 1.f, 0.f);

		// Four triangles around a bright centre: a cheap round-ish glow.
		const SDL_FPoint c = Pt(x, y);
		const SDL_FPoint n = Pt(x, y - s), e = Pt(x + s, y);
		const SDL_FPoint s2 = Pt(x, y + s), wpt = Pt(x - s, y);
		b.Tri(c, n, e, core, edge, edge);
		b.Tri(c, e, s2, core, edge, edge);
		b.Tri(c, s2, wpt, core, edge, edge);
		b.Tri(c, wpt, n, core, edge, edge);
	}
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

void Visualizer::Shutdown()
{
	if (target_)
	{
		SDL_DestroyTexture(target_);
		target_ = nullptr;
	}
	tw_ = th_ = 0;
	target_failed_ = false;
}

bool Visualizer::EnsureTarget(SDL_Renderer* renderer, int w, int h)
{
	if (target_failed_)
		return false;

	const int scale = std::max(1, (int)((float)h / kTargetHeight + 0.5f));
	const int tw = std::max(64, w / scale);
	const int th = std::max(36, h / scale);
	if (target_ && tw == tw_ && th == th_)
		return true;

	if (target_)
		SDL_DestroyTexture(target_);
	target_ = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
	                            SDL_TEXTUREACCESS_TARGET, tw, th);
	if (!target_)
	{
		// No render targets: fall back to leaving the background alone
		// rather than tanking the frame rate drawing this at full size.
		SDL_Log("visualizer: no render target (%s); animation disabled", SDL_GetError());
		target_failed_ = true;
		tw_ = th_ = 0;
		return false;
	}
	// Opaque blit on the way out, so the upscale takes SDL's fast path.
	SDL_SetTextureBlendMode(target_, SDL_BLENDMODE_NONE);
	SDL_SetTextureScaleMode(target_, SDL_ScaleModeNearest);
	tw_ = tw;
	th_ = th;
	return true;
}

void Visualizer::Render(SDL_Renderer* renderer, int w, int h, int64_t clock_us, bool paused)
{
	if (!renderer || w <= 0 || h <= 0)
		return;

	const uint64_t now = SDL_GetPerformanceCounter();
	float dt = 1.f / 60.f;
	if (last_tick_ != 0)
		dt = (float)((double)(now - last_tick_) / (double)SDL_GetPerformanceFrequency());
	last_tick_ = now;
	// A long stall (loading, seeking) must not teleport the animation.
	dt = std::min(std::max(dt, 0.f), 0.1f);

	Analyze(clock_us, dt, paused);
	UpdateMotion(dt);

	if (!EnsureTarget(renderer, w, h))
		return;

	SDL_Texture* prev_target = SDL_GetRenderTarget(renderer);
	SDL_BlendMode prev_blend = SDL_BLENDMODE_BLEND;
	SDL_GetRenderDrawBlendMode(renderer, &prev_blend);
	SDL_SetRenderTarget(renderer, target_);

	// Layers are sized from the surface they draw onto, so working at
	// quarter scale needs no other change.
	DrawBackdrop(renderer, tw_, th_);

	// Everything above the backdrop is light, so it all adds. Same blend
	// mode and no texture for every layer, which collapses to one draw call.
	SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_ADD);
	Batch b;
	DrawMotes(b, tw_, th_);
	DrawBars(b, tw_, th_);
	b.Flush(renderer);

	SDL_SetRenderTarget(renderer, prev_target);
	SDL_SetRenderDrawBlendMode(renderer, prev_blend);
	SDL_RenderCopy(renderer, target_, nullptr, nullptr);
}

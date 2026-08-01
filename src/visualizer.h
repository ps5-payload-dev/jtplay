// SPDX-License-Identifier: GPL-3.0-or-later
//
// Audio spectrum visualizer for audio-only playback, where there is no video
// plane to draw and the background would otherwise stay black.
//
// The audio thread hands over the same S16 stereo blocks it queues to the
// SDL audio device, tagged with the timestamp at the end of each block. That
// is exactly what Player::MasterClock() counts down from, so the renderer can
// look up the samples that are *audible right now* rather than the ones most
// recently decoded -- up to two seconds sit queued in the audio device, and
// animating to those would visibly lead the music.
//
// Everything is drawn with SDL_RenderGeometry, matching the rest of the
// player: no OpenGL, no shaders, one draw call for the additive layers.
//
// The backend runs SDL_RENDERER_SOFTWARE, so every pixel is blended on the
// CPU and fill rate, not geometry, is the whole cost. It is therefore drawn
// into a quarter-scale target and upscaled with a single opaque blit, which
// cuts the blended pixel count by 16. Measured offscreen at 1920x1080:
// ~3.9 ms to draw plus ~1.6 ms to blit, against ~50 ms drawing the same
// layers straight to the backbuffer.
//
// Note the software renderer only scales nearest-neighbour; the linear mode
// requested below is silently ignored and only takes effect if this ever
// runs on an accelerated renderer. At a 4x integer upscale of what is all
// soft gradients, nearest is not visibly worse.
#ifndef VISUALIZER_H
#define VISUALIZER_H

#include <cstdint>
#include <mutex>
#include <vector>

#include <SDL.h>

// PCM format accepted by Push(); matches the player's resampler output.
inline constexpr int kVizRate = 48000;
inline constexpr int kVizChannels = 2;

// Log-spaced frequency bands driving the animation.
inline constexpr int kVizBands = 64;

class Visualizer
{
public:
	Visualizer();

	// --- audio thread -------------------------------------------------------

	// Takes a block of interleaved S16 stereo samples at kVizRate.
	// 'pts_end_us' is the presentation timestamp one sample past the end of
	// the block (the same value stored in Player::audio_pts_end_).
	void Push(const int16_t* interleaved, int frames, int64_t pts_end_us);

	// Drops buffered audio and settles the animation. Called on seek, on a
	// track switch and when the pipeline stops, so the visualizer never
	// animates to samples from before the jump.
	void Reset();

	// Releases the offscreen target. Must be called while the renderer is
	// still alive; the destructor deliberately does not touch SDL.
	void Shutdown();

	// --- main thread --------------------------------------------------------

	// Draws a full-screen animation for the given master clock (INT64_MIN
	// when playback has not started). The renderer's draw blend mode is
	// restored before returning, so the UI layer composites as usual.
	void Render(SDL_Renderer* renderer, int w, int h, int64_t clock_us, bool paused);

private:
	// Accumulates coloured triangles so a whole layer is one SDL_RenderGeometry
	// call. Untextured geometry uses the renderer's draw blend mode.
	struct Batch
	{
		std::vector<SDL_Vertex> verts;
		std::vector<int> indices;

		void Reset();
		// Corners in order: top-left, top-right, bottom-right, bottom-left.
		void Quad(const SDL_FPoint p[4], const SDL_Color c[4]);
		void Tri(SDL_FPoint a, SDL_FPoint b, SDL_FPoint c,
		         SDL_Color ca, SDL_Color cb, SDL_Color cc);
		void Flush(SDL_Renderer* renderer);
	};

	// Drifting mote, nudged upward and outward by beats.
	struct Mote
	{
		float x = 0.f, y = 0.f;   // normalized to the window
		float vx = 0.f, vy = 0.f;
		float size = 0.f;         // normalized to window height
		float phase = 0.f;        // twinkle offset
		float band = 0.f;         // which band it listens to, 0..1
	};

	// Fills band_ from the samples audible at 'clock_us'.
	void Analyze(int64_t clock_us, float dt, bool paused);
	// Copies the analysis window out of the ring buffer. Returns false when
	// there is not enough history yet (start of a track, after a seek).
	bool FetchWindow(int64_t clock_us, float* out);
	void UpdateMotion(float dt);
	// (Re)creates the offscreen target for a window of w x h.
	bool EnsureTarget(SDL_Renderer* renderer, int w, int h);

	void DrawBackdrop(SDL_Renderer* renderer, int w, int h);
	void DrawBars(Batch& b, int w, int h);
	void DrawMotes(Batch& b, int w, int h);

	// --- shared with the audio thread ---------------------------------------
	static constexpr int kFFT = 2048;              // ~43 ms window, 23 Hz bins
	static constexpr int kHistory = kVizRate * 4;  // 4 s, covers the audio queue

	mutable std::mutex mutex_;
	std::vector<float> history_;      // mono, ring buffer of kHistory samples
	int64_t written_ = 0;             // total samples ever written since Reset
	int64_t pts_end_us_ = INT64_MIN;  // timestamp of sample 'written_'

	// --- analysis (main thread) ---------------------------------------------
	std::vector<float> window_;   // Hann coefficients
	std::vector<float> re_, im_;
	std::vector<int> rev_;        // bit-reversal permutation
	std::vector<float> tw_cos_, tw_sin_;
	std::vector<int> band_lo_, band_hi_;  // bin range per band

	float band_[kVizBands] = {};  // smoothed magnitudes, 0..1
	float peak_[kVizBands] = {};  // peak-hold caps
	float energy_ = 0.f;          // broadband level, 0..1
	float bass_ = 0.f;            // low-band level, 0..1
	float bass_avg_ = 0.f;        // running mean, for beat detection
	float centroid_ = 0.5f;       // spectral centroid, 0..1; drives the hue
	float beat_flash_ = 0.f;      // decays from 1 on each beat
	float since_beat_ = 0.f;
	bool  silent_ = true;         // no usable audio: run the idle animation

	// --- motion (main thread) -----------------------------------------------
	float time_ = 0.f;
	std::vector<Mote> motes_;
	uint32_t rng_ = 0x9e3779b9u;

	// --- offscreen target ---------------------------------------------------
	SDL_Texture* target_ = nullptr;
	int tw_ = 0, th_ = 0;
	bool target_failed_ = false;

	uint64_t last_tick_ = 0;      // SDL performance counter

	float Rand();
};

#endif

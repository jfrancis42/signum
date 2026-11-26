// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// waterfall.hpp: a waterfall of the station's own receive audio (TODO 6,
// decision 10, user 2026-10-23: from the audio, so it works with any radio,
// puts no load on CAT, and shows exactly the passband the modems hear).
//
// Input: the 9600 Hz receive stream, one sample at a time (put()). Output:
// rows of spectrum, 0 .. kMaxHz, one byte per bin: 0.5 dB steps,
// 0 = -127.5 dBFS and below, 255 = 0 dBFS (a full-scale sine). The station
// sends them base64-encoded over the WebSocket (src/web/README.md).
//
// No heap, no OS calls: fixed arrays, a radix-2 float FFT, a Hann window
// (portable to the ESP32 as it stands). Cost per row: one 1024-point FFT
// and kBins log10s; at 10 rows/s that is well under 1 % of a Pi 4 core.
#pragma once
#include <cmath>
#include <cstdint>
#include <string>

namespace wfall {

class Waterfall {
public:
	static constexpr int kFs = 9600, kN = 1024;
	static constexpr double kBinHz = double(kFs) / kN;          // 9.375 Hz
	static constexpr int kMaxHz = 4000;
	static constexpr int kBins = int(kMaxHz / kBinHz) + 1;      // 0 .. 4000 Hz: 427 bins

	explicit Waterfall(int rows_per_s = 10) { set_rate(rows_per_s); init(); }
	void set_rate(int rows_per_s) { hop_ = kFs / (rows_per_s < 1 ? 1 : rows_per_s > 40 ? 40 : rows_per_s); }
	int rate() const { return kFs / hop_; }
	void reset() { pos_ = 0; filled_ = 0; since_ = 0; ready_ = false; }

	// One receive sample. Returns true when a new row is ready (row()).
	bool put(int16_t s)
	{
		ring_[pos_] = float(s);
		pos_ = (pos_ + 1) & (kN - 1);
		if (filled_ < kN) filled_++;
		if (++since_ >= hop_ && filled_ == kN) { since_ = 0; compute(); ready_ = true; return true; }
		return false;
	}
	bool ready() const { return ready_; }
	const uint8_t *row() { ready_ = false; return row_; }

	// The row as base64 (the WebSocket's JSON carries it as a string).
	static std::string base64(const uint8_t *p, int n)
	{
		static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		std::string o;
		o.reserve(size_t((n + 2) / 3 * 4));
		for (int i = 0; i < n; i += 3) {
			const uint32_t v = uint32_t(p[i]) << 16 | (i + 1 < n ? uint32_t(p[i + 1]) << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
			o += t[v >> 18 & 63]; o += t[v >> 12 & 63];
			o += i + 1 < n ? t[v >> 6 & 63] : '=';
			o += i + 2 < n ? t[v & 63] : '=';
		}
		return o;
	}

private:
	float ring_[kN] = {}, win_[kN] = {}, re_[kN] = {}, im_[kN] = {}, cs_[kN / 2] = {}, sn_[kN / 2] = {};
	uint16_t rev_[kN] = {};
	uint8_t row_[kBins] = {};
	int pos_ = 0, filled_ = 0, since_ = 0, hop_ = 960;
	bool ready_ = false;

	void init()
	{
		const double pi = 3.14159265358979323846;
		for (int i = 0; i < kN; i++) win_[i] = float(0.5 - 0.5 * std::cos(2 * pi * i / kN));
		for (int i = 0; i < kN / 2; i++) { cs_[i] = float(std::cos(2 * pi * i / kN)); sn_[i] = float(-std::sin(2 * pi * i / kN)); }
		int bits = 0;
		while ((1 << bits) < kN) bits++;
		for (int i = 0; i < kN; i++) { int r = 0; for (int b = 0; b < bits; b++) if (i >> b & 1) r |= 1 << (bits - 1 - b); rev_[i] = uint16_t(r); }
	}
	void compute()
	{
		for (int i = 0; i < kN; i++) { const int k = rev_[i]; re_[k] = ring_[(pos_ + i) & (kN - 1)] * win_[i]; im_[k] = 0; }   // oldest first
		for (int len = 2; len <= kN; len <<= 1) {
			const int half = len / 2, step = kN / len;
			for (int i = 0; i < kN; i += len)
				for (int j = 0; j < half; j++) {
					const float wr = cs_[j * step], wi = sn_[j * step];
					const int a = i + j, b = a + half;
					const float tr = re_[b] * wr - im_[b] * wi, ti = re_[b] * wi + im_[b] * wr;
					re_[b] = re_[a] - tr; im_[b] = im_[a] - ti;
					re_[a] += tr; im_[a] += ti;
				}
		}
		// A full-scale sine (amplitude 32767) through the Hann window peaks at 32767 * N / 4: 0 dBFS.
		const float ref = 32767.0f * kN / 4, ref2 = ref * ref;
		for (int k = 0; k < kBins; k++) {
			const float p = (re_[k] * re_[k] + im_[k] * im_[k]) / ref2;
			const float db = p > 1e-20f ? 10.0f * std::log10(p) : -200.0f;
			const float v = (db + 127.5f) * 2.0f;
			row_[k] = uint8_t(v < 0 ? 0 : v > 255 ? 255 : v + 0.5f);
		}
	}
};

}  // namespace wfall

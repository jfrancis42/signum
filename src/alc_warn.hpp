// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// alc_warn.hpp: warn when the radio's ALC acts while we send P2/P3 (--alc-warn, ALCwarn). Warn only: this
// never changes the TX gain, PSKAmpl or the radio's power (lowering the drive is the owner's decision).
//
// Why (on the air 2026-11-20, radio 1, 80 m, 25 W limit): TX gain +4 dB against the limit squashed the PSK
// peaks; the RMS answered P (repeat) to every break-in and the link stayed on SL1. At TX gain -2 dB it got S
// and SL3. Nothing in Signum noticed. The radio's ALC meter (Hamlib `l ALC`, 0..1) shows the squashing.
//
// This file is the judging, with no OS calls (the ESP32 rule). The reading is an AlcReader: on the program's
// side src/radio/rig_alc.hpp, a worker thread with a rigctld connection of its own, that polls "l ALC" while
// the transmitter is keyed. The audio loop only tells it when we key (key()) and takes what it has read
// (service(), never waiting).
//
// What is judged: one burst = one key-down of a P2/P3 link (P1 and anything outside a link is not read).
// A burst's value is the highest ALC read in it; a burst with fewer than kMinReads readings (an
// acknowledgment, a key-down shorter than about a second) is not counted. A burst is "over" when its value
// is at or above kWarnAlc. The warning comes once the last kWindow counted bursts hold kOverNeeded over
// ones, once per link; at the link's end one summary line follows (bursts read, highest ALC, how many over).
//
// The threshold [inferred]: on radio 1 P3 at TX gain -2 dB read ALC 0.20 to 0.44 of 1 (owner's reading,
// 2026-11-20: "still a little hot") and the link ran at SL3 with the RMS answering S, so readings up to 0.44
// are tolerated by the far end. kWarnAlc = 0.50 sits just above the highest reading of a link that worked.
// The squashed +4 dB case was not logged against the meter, so it is not known where it read; Hamlib's
// IC-7300 ALC scale puts the meter's full scale at 1.0, so +4 dB over the limit must read above the -2 dB
// figure by the 6 dB between the two settings. kOverNeeded 3 of kWindow 6 so that one spike (a key click, a
// reading taken at the moment of keying) cannot warn, while a drive that is wrong on every burst warns after
// three. A measured P2/P3 link on another radio may need other numbers: they are constants here, listed in
// docs/md/radio-ic7300.md "ALC warning", and not options.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

namespace signum {

struct AlcReading {
	double alc = 0;               // 0..1, as the radio reports it
	unsigned burst = 0;           // the key-down it was read in (AlcWarn::key's id)
};

// The program's adapter to the radio's ALC meter (src/radio/rig_alc.hpp). Never blocks the caller.
class AlcReader {
public:
	virtual ~AlcReader() {}
	// The transmitter is keyed (true) or not (false); burst: the id of this key-down (rises with each one).
	virtual void keyed(bool k, unsigned burst) = 0;
	// What has been read since the last call (false: nothing, or the reader is busy: try again next period).
	virtual bool drain(std::vector<AlcReading> &out) = 0;
	// 0 not known yet (nothing tried), 1 reading works, 2 not available (`why`: the reason). Not blocking.
	virtual int state(std::string &why) = 0;
};

class AlcWarn {
public:
	// the judging (see the top of this file for how they were chosen)
	static constexpr double kWarnAlc = 0.50;
	static constexpr int kMinReads = 2, kWindow = 6, kOverNeeded = 3;

	bool enabled = true;          // --alc-warn / ALCwarn
	AlcReader *reader = nullptr;  // null: no rigctld PTT

	// The audio loop's key-down / key-up (our transmitter). plevel: the link's P level (0 = no link), level: its
	// speed level, gain_db: the TX gain now.
	void key(bool keyed, int plevel, int level, double gain_db, double t)
	{
		if (keyed) {
			if (!enabled || plevel < 2) { cur_on_ = false; return; }
			cur_on_ = true;
			cur_id_++;
			ctx_ = Ctx{plevel, level, gain_db};
			if (reader) reader->keyed(true, cur_id_);
		} else if (cur_on_) {
			cur_on_ = false;
			closed_id_ = cur_id_; closed_t_ = t;
			if (reader) reader->keyed(false, cur_id_);
		}
	}

	// Once per audio period: the readings, the link's start and end. t: audio time (s).
	void service(double t, bool link_up, int plevel)
	{
		if (link_up != up_) {
			up_ = link_up;
			if (up_) begin(plevel); else end();
		}
		if (const std::string s = state(); s != last_state_) { last_state_ = s; dirty_ = true; }
		if (!reader) return;
		if (std::vector<AlcReading> r; reader->drain(r)) for (const auto &x : r) read(x);
		if (acc_.n >= 0 && closed_id_ == acc_.id && t - closed_t_ > 0.5) finish_burst();
		std::string why;
		const int s = reader->state(why);
		if (s == 2 && !unavail_said_ && enabled && up_) {
			unavail_said_ = true; dirty_ = true;
			log_ += "[alc] not available: " + why + "; no ALC warning on this run\n";
		}
		if (s != rstate_) { rstate_ = s; dirty_ = true; }
	}

	// Log lines made since the last call.
	std::string take_log() { std::string l; l.swap(log_); return l; }
	bool take_dirty() { const bool d = dirty_; dirty_ = false; return d; }
	bool warned() const { return warned_; }
	int bursts() const { return bursts_; }
	int over() const { return over_; }
	double max_alc() const { return max_; }

	// "off" (--alc-warn off), "n/a" (no rigctld PTT, or the radio does not give an ALC reading), "ok", "warn"
	const char *state() const
	{
		if (!enabled) return "off";
		if (!reader || rstate_ == 2) return "n/a";
		return warned_ ? "warn" : "ok";
	}
	// The panel's "alc" and the console's text.
	std::string json() const
	{
		char b[320];
		std::snprintf(b, sizeof b, "{\"state\":\"%s\",\"max\":%.2f,\"bursts\":%d,\"over\":%d,\"limit\":%.2f,\"on_link\":%d,\"mode\":%d,\"level\":%d,\"gain\":%.1f}",
		              state(), bursts_ ? max_ : 0.0, bursts_, over_, kWarnAlc, up_ ? 1 : 0, warned_ ? w_.plevel : 0, warned_ ? w_.level : 0, warned_ ? w_.gain : 0.0);
		return b;
	}
	std::string text() const
	{
		char b[240];
		const std::string st = state();
		const char *what = st == "off" ? "OFF" : st == "n/a" ? (reader ? "NOT AVAILABLE (THE RADIO GIVES NO ALC READING)" : "NOT AVAILABLE (THE PTT IS NOT RIG, A RIGCTLD)") : st == "warn" ? "ON, WARNING GIVEN THIS LINK" : "ON";
		if (bursts_) std::snprintf(b, sizeof b, "*** ALC WARNING (0=OFF/1=ON): %d  (%s; THIS LINK: %d BURSTS READ, HIGHEST ALC %.2f, %d OVER %.2f)", enabled ? 1 : 0, what, bursts_, max_, over_, kWarnAlc);
		else std::snprintf(b, sizeof b, "*** ALC WARNING (0=OFF/1=ON): %d  (%s)", enabled ? 1 : 0, what);
		return b;
	}

private:
	struct Ctx { int plevel = 0, level = 0; double gain = 0; };
	struct Acc { int n = -1; unsigned id = 0; double max = 0; };   // n -1: none open
	void begin(int plevel)
	{
		bursts_ = 0; over_ = 0; max_ = 0; warned_ = false; recent_.clear(); acc_ = Acc(); closed_id_ = 0; dirty_ = true;
		if (enabled && plevel >= 2 && !reader && !nord_said_) {
			nord_said_ = true;
			log_ += "[alc] not available: the PTT is not rig:HOST:PORT (rigctld), so the ALC meter cannot be read; no ALC warning on this run\n";
		}
	}
	void end()
	{
		finish_burst();
		if (bursts_ > 0) {
			char b[240];
			std::snprintf(b, sizeof b, "[alc] link summary: %d bursts read, highest ALC %.2f, %d at or above %.2f (%.0f %%)%s\n", bursts_, max_, over_, kWarnAlc,
			              100.0 * over_ / bursts_, warned_ ? "; the warning was given" : "");
			log_ += b;
		}
		dirty_ = true;
	}
	void read(const AlcReading &x)
	{
		if (acc_.n >= 0 && acc_.id != x.burst) finish_burst();
		if (acc_.n < 0) { acc_ = Acc(); acc_.n = 0; acc_.id = x.burst; }
		acc_.n++; acc_.max = std::max(acc_.max, x.alc);
	}
	void finish_burst()
	{
		if (acc_.n < 0) return;
		const Acc a = acc_;
		acc_ = Acc();
		if (a.n < kMinReads || !up_) return;
		const bool over = a.max >= kWarnAlc;
		bursts_++; over_ += over ? 1 : 0; max_ = std::max(max_, a.max);
		recent_.push_back({a.max, over});
		if (int(recent_.size()) > kWindow) recent_.pop_front();
		dirty_ = true;
		int k = 0; for (const auto &r : recent_) k += r.over ? 1 : 0;
		if (!warned_ && enabled && k >= kOverNeeded) {
			warned_ = true; w_ = ctx_;
			std::string rd;
			for (const auto &r : recent_) { char c[16]; std::snprintf(c, sizeof c, "%s%.2f", rd.empty() ? "" : " ", r.max); rd += c; }
			char b[480];
			std::snprintf(b, sizeof b, "[alc] WARNING: the radio's ALC is acting on our P%d transmissions (level %d, TX gain %+.1f dB): %d of the last %d bursts reached ALC %.2f or more (highest of each: %s). "
			              "The PSK peaks are probably being squashed: the other station may answer P (repeat) and hold the link at a low level. Lower the TX gain (--tx-gain, or the GUI's Configure tab) or the radio's RF POWER; Signum changes nothing by itself.\n",
			              w_.plevel, w_.level, w_.gain, k, int(recent_.size()), kWarnAlc, rd.c_str());
			log_ += b;
		}
	}

	struct Rec { double max; bool over; };
	std::deque<Rec> recent_;
	Ctx ctx_, w_;
	Acc acc_;
	std::string log_, last_state_;
	unsigned cur_id_ = 0, closed_id_ = 0;
	double closed_t_ = 0, max_ = 0;
	int bursts_ = 0, over_ = 0, rstate_ = 0;
	bool cur_on_ = false, up_ = false, warned_ = false, dirty_ = false, unavail_said_ = false, nord_said_ = false;
};

}  // namespace signum

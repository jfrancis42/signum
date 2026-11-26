// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// tune_tone.hpp: the tuning tone before a new call (an option, off by default).
//
// Before the first call attempt of a new outgoing connect (C, host-mode C, the GUI's
// Connect; not the modem's own call repeats, not as callee, not for unproto) the station
// keys and sends one steady sine for `secs` seconds so an automatic antenna tuner can
// tune, unkeys, waits kGapS of silence, then the call goes out as without the option.
// The far station never hears anything of the protocol in it: no compatibility effect.
//
// Level: `db` relative to the station's data level (P2Config::amplitude, which PSKAmpl
// and --tx-gain set: the peak of our P1 FSK and the base of every PSK level), never
// above it. 0 dB = the same peak and the same average power as our P1 call packets
// (FSK is constant-envelope); see docs/md/radio-setup.md "Tuning tone" for the numbers.
//
// The tone starts and ends with a 10 ms raised-cosine ramp (no key clicks); an abort
// (D, DD, QRT, the QRT character, the GUI's Disconnect) ramps down in 5 ms and unkeys,
// and the call is dropped. The key is never held longer than `secs` (plus the audio
// already queued in the sound card when the caller unkeys).
//
// Tune method (0.7.0): `method` AUTO, TONE or RADIO. With RADIO or AUTO and a RadioTuner (the program's
// adapter to the radio's own tune cycle through rigctld, src/radio/rig_tune.hpp) the radio tunes itself
// instead of the tone: for up to `secs` seconds the station waits in silence (it does NOT key: the radio
// transmits its own carrier at its own RF POWER), then the 0.3 s gap and the call, as with the tone. The
// RadioTuner says at once if it cannot (no rigctld control, the rig has no TUNE op) and later if the radio
// refuses; either way the tone is sent instead and the log says why: never silently nothing (except
// method TONE, which is the tone as before). An abort during the wait drops the call the same way;
// the radio finishes the cycle it was told to start (a tuner stops itself).
//
// Portable: no OS calls, no heap in the sample path (the ESP32 rule).
#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>

namespace signum {

// The program's adapter to the radio's own tune cycle (src/radio/rig_tune.hpp). Never blocks.
class RadioTuner {
public:
	virtual ~RadioTuner() {}
	// Can a radio tune be tried now? False: why (no rigctld control, the rig lists no TUNE op, ...).
	virtual bool available(std::string &why) = 0;
	// Begin the cycle and the wait of up to `secs` s; poll() tells how it went.
	virtual void start(int secs) = 0;
	// 0 under way; 1 done (`text`: what was seen); 2 failed before the radio tuned (`text`: why): the tone follows.
	virtual int poll(std::string &text) = 0;
	// Stop waiting (the call is dropped). The radio itself is not told to stop.
	virtual void cancel() = 0;
};

class TuneTone {
public:
	enum Method { AUTO = 0, TONE_ONLY = 1, RADIO = 2 };
	static const char *method_name(int m) { return m == TONE_ONLY ? "tone" : m == RADIO ? "radio" : "auto"; }
	static bool parse_method(const std::string &w, int &m)
	{
		std::string u; for (char c : w) u += char(std::tolower(static_cast<unsigned char>(c)));
		if (u == "auto") m = AUTO; else if (u == "tone") m = TONE_ONLY; else if (u == "radio") m = RADIO; else return false;
		return true;
	}
	int method = AUTO;
	RadioTuner *radio = nullptr;  // the radio's own tune cycle, when this program has rigctld control
	static constexpr double kRadioSlackS = 12.0;   // the radio path is given up this long after `secs` (rigctld not answering)

	// the settings' limits and defaults
	static constexpr int kMaxS = 10, kMinHz = 300, kMaxHz = 2700, kDefHz = 1500, kMinDb = -30, kMaxDb = 0, kDefDb = 0;
	static constexpr double kRampS = 0.010, kAbortRampS = 0.005, kGapS = 0.3;
	int secs = 0;                 // 0 = off (the default: the reference unit sends no such tone)
	int hz = kDefHz;
	int db = kDefDb;

	static bool valid_secs(int v) { return v >= 0 && v <= kMaxS; }
	static bool valid_hz(int v) { return v >= kMinHz && v <= kMaxHz; }
	static bool valid_db(int v) { return v >= kMinDb && v <= kMaxDb; }

	bool active() const { return ph_ != IDLE; }   // the tone or the gap after it: the call is under way
	bool keyed() const { return ph_ == TONE; }
	bool radio_tuning() const { return ph_ == WAIT_RADIO; }   // the radio's own tune cycle is under way (not keyed by us)

	// Start the tuning for `call` (amplitude: the station's data peak level; fs: its sample rate): the radio's own
	// tune cycle when the method allows it and the program can, else the tone.
	void start(const std::string &call, float amplitude, float fs)
	{
		call_ = call;
		fs_ = double(fs);
		amp_ = double(amplitude) * std::pow(10.0, double(db) / 20.0);
		if (amp_ > 32000.0) amp_ = 32000.0;
		ready_ = false; aborted_ = false; abort_i_ = -1; abort_env_ = 0; i_ = 0;
		std::string why;
		if (method == TONE_ONLY) { notes_.push_back("tune: tone (--tune-method tone)"); begin_tone(); return; }
		if (!radio) why = "no rigctld control: the PTT is not rig";
		if (radio && radio->available(why)) {
			n_ = long(std::lround((double(secs) + kRadioSlackS) * fs_));
			gap_ = long(std::lround(kGapS * fs_));
			ph_ = WAIT_RADIO;
			radio->start(secs);
			notes_.push_back("tune: radio (" + (why.empty() ? std::string("asking rigctld whether the rig supports TUNE") : why) + "), up to " + std::to_string(secs) + " s before calling " + call_ +
			                 ": the radio transmits at its own RF POWER, this station does not key");
			return;
		}
		notes_.push_back(method == RADIO ? "tune: radio requested but not possible (" + why + "); using the tone" : "tune: tone (" + why + ")");
		begin_tone();
	}
	// The radio path's progress: call once per audio period (the program's loop, not the sample path).
	void service()
	{
		if (ph_ != WAIT_RADIO || !radio) return;
		std::string t;
		const int r = radio->poll(t);
		if (r == 1) { notes_.push_back("tune: radio tune over (" + t + "): calling " + call_ + " in " + "0.3 s"); ph_ = GAP; i_ = 0; }
		else if (r == 2) { notes_.push_back(method == RADIO ? "tune: radio requested but not possible (" + t + "); using the tone" : "tune: tone (" + t + ")"); begin_tone(); }
	}
	// One sample (the tone, or silence in the gap); 0 when idle.
	int16_t tick()
	{
		if (ph_ == GAP) {
			if (++i_ >= gap_) { ph_ = IDLE; ready_ = true; }
			return 0;
		}
		if (ph_ == WAIT_RADIO) {               // silence; given up when the radio path never answers
			if (++i_ >= n_) { if (radio) radio->cancel(); notes_.push_back(method == RADIO ? "tune: radio requested but not possible (rigctld did not answer in time); using the tone" : "tune: tone (rigctld did not answer in time)"); begin_tone(); }
			return 0;
		}
		if (ph_ != TONE) return 0;
		double env = 1.0;
		if (abort_i_ >= 0) {
			const long k = i_ - abort_i_, ra = long(std::lround(kAbortRampS * fs_));
			env = k >= ra ? 0.0 : abort_env_ * 0.5 * (1.0 + std::cos(M_PI * double(k) / double(ra)));
			if (k >= ra) { end_tone(); return 0; }
		} else env = env_at(i_);
		const double y = amp_ * env * std::sin(phi_);
		phi_ += dphi_;
		if (phi_ > 2.0 * M_PI) phi_ -= 2.0 * M_PI;
		if (++i_ >= n_ && abort_i_ < 0) {
			char b[96];
			std::snprintf(b, sizeof b, "tuning tone ended after %.2f s: unkeyed, calling %s in %.1f s", double(n_) / fs_, call_.c_str(), kGapS);
			notes_.push_back(b);
			ph_ = GAP; i_ = 0;
		}
		return int16_t(std::lround(y));
	}
	// Abort (the call is dropped): ramps down in 5 ms and unkeys. False when there was nothing to abort.
	bool cancel()
	{
		if (ph_ == IDLE || aborted_) return false;
		aborted_ = true;
		char b[176];
		std::snprintf(b, sizeof b, "tuning tone aborted after %.2f s: unkeyed, the call to %s dropped", ph_ == TONE ? double(i_) / fs_ : double(n_) / fs_, call_.c_str());
		if (ph_ == WAIT_RADIO) {
			if (radio) radio->cancel();
			std::snprintf(b, sizeof b, "tune: aborted during the radio tune: the call to %s dropped (the radio finishes its own cycle)", call_.c_str());
		}
		notes_.push_back(b);
		if (ph_ == TONE) { abort_i_ = i_; abort_env_ = env_at(i_); }
		else end_tone();
		return true;
	}
	// The tone and the gap are over (not aborted): the call to place now.
	bool take_call(std::string &call) { if (!ready_) return false; ready_ = false; call = call_; return true; }
	// An abort to report as the call's end (once): the call it dropped.
	bool take_aborted(std::string &call) { if (!aborted_ || ph_ != IDLE) return false; aborted_ = false; call = call_; return true; }
	// A line for the log on each change (keyed, ended, aborted), once.
	bool take_note(std::string &s) { if (notes_.empty()) return false; s = notes_.front(); notes_.pop_front(); return true; }

	std::string upper_method() const { std::string u = method_name(method); for (auto &c : u) c = char(std::toupper(static_cast<unsigned char>(c))); return u; }
	std::string db_text() const { char b[16]; std::snprintf(b, sizeof b, "%d dB", db); return b; }
	// The interpreter's answer (TUnetone).
	std::string text() const
	{
		return "*** TUNING TONE BEFORE A NEW CALL: " + (secs ? std::to_string(secs) + " S" : std::string("0 (OFF)")) + ", " + std::to_string(hz) + " HZ, " +
		       std::to_string(db) + " DB, METHOD " + upper_method() + "\r\n";
	}

private:
	enum Phase { IDLE, TONE, GAP, WAIT_RADIO };
	void begin_tone()
	{
		n_ = long(std::lround(double(secs) * fs_));
		ramp_ = std::max(1L, long(std::lround(kRampS * fs_)));
		gap_ = long(std::lround(kGapS * fs_));
		dphi_ = 2.0 * M_PI * double(hz) / fs_;
		phi_ = 0; i_ = 0; abort_i_ = -1; abort_env_ = 0; ready_ = false;
		ph_ = n_ > 0 ? TONE : GAP;
		notes_.push_back("tuning tone " + std::to_string(hz) + " Hz, " + db_text() + ", " + std::to_string(secs) + " s before calling " + call_ + ": keyed");
	}
	double env_at(long i) const
	{
		const long r = std::min(ramp_, n_ / 2 > 0 ? n_ / 2 : 1);
		if (i < r) return 0.5 - 0.5 * std::cos(M_PI * double(i) / double(r));
		if (i >= n_ - r) return 0.5 - 0.5 * std::cos(M_PI * double(n_ - i) / double(r));
		return 1.0;
	}
	void end_tone() { ph_ = IDLE; i_ = 0; abort_i_ = -1; }
	Phase ph_ = IDLE;
	std::string call_;
	std::deque<std::string> notes_;
	double fs_ = 9600, amp_ = 0, dphi_ = 0, phi_ = 0, abort_env_ = 0;
	long n_ = 0, ramp_ = 1, gap_ = 0, i_ = 0, abort_i_ = -1;
	bool aborted_ = false, ready_ = false;
};

}  // namespace signum

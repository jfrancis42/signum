// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// cpu_health.hpp: "is this computer keeping up?" as one state, OK / BUSY / STARVED.
//
// The host reports every sign of lateness it sees (the audio loop's playback underruns and capture
// stalls, the modem's late decodes, thin decode margins, computer back-off steps, the SL1 rescue,
// late transmit audio) with the time it happened. The state is a decayed score of the events of the
// last minute, with hysteresis and a minimum dwell, so it neither flaps nor shouts at one hiccup:
//
//   * the first kGraceS seconds after the loop starts are ignored (start-up hiccups);
//   * BUSY needs a score of 2.5 AND at least 2 events in the window; STARVED 7 AND at least 4;
//   * a state is left only when the score has fallen to half its entry value, and not before
//     kMinDwellS seconds in it. The score decays with a time constant of kTauS (25 s), so an
//     isolated event is forgotten in about a minute.
//
// The state changes nothing by itself: with the single CPUadapt setting on, the modem drops CPU-bound
// extras while it is not OK (its CPU fallbacks, reported here: set_fallbacks).
// Two static conditions are carried along because they decide the advice, not the state: no
// real-time priority, and a CPU clock that cannot be held up.
//
// Pure logic: time comes in as a parameter, no OS calls (the ESP32 rule; src/signum.cpp does the
// measuring and the /proc read).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

namespace signum {

class CpuHealth {
public:
	enum State { OK = 0, BUSY = 1, STARVED = 2 };
	enum Kind { UNDERRUN, STALL, LATE, THIN, BACKOFF, SL1, TXLATE, NKINDS };

	static constexpr double kTauS = 25.0;        // score decay
	static constexpr double kWindowS = 60.0;     // events kept and counted
	static constexpr double kGraceS = 20.0;      // start-up: events before this are ignored
	static constexpr double kMinDwellS = 10.0;   // least time in a state before a lower one
	static constexpr double kBusyScore = 2.5, kStarvedScore = 7.0;
	static constexpr int kBusyCount = 2, kStarvedCount = 4;

	// Static conditions (set once at start-up).
	void set_no_realtime(bool v) { no_rt_ = v; }
	void set_clock_unpinned(bool v, const std::string &why = "") { clock_ = v; clock_why_ = why; }
	// The computer's load average over 1 minute and its core count (the host reads them when the state
	// leaves OK); 0 cores: unknown.
	void set_load(double load1, int cores) { load1_ = load1; cores_ = cores; }
	// The CPU fallbacks in use (the modem's, with CPUadapt: scs-modem TODO 5), as short phrases. A change
	// is logged once ("[cpu] fallback on: ..." / "[cpu] fallbacks off ..."); they are listed by the console
	// and the panel. They are not lateness: they never raise the state.
	bool set_fallbacks(const std::vector<std::string> &f)   // true: changed (publish the panel again)
	{
		if (f == fb_) return false;
		std::string l;
		for (const auto &x : f)
			if (std::find(fb_.begin(), fb_.end(), x) == fb_.end()) l += (l.empty() ? "" : "; ") + x;
		if (!l.empty()) fb_log_ += "[cpu] fallback on (CPUadapt): " + l + "\n";
		std::string o;
		for (const auto &x : fb_)
			if (std::find(f.begin(), f.end(), x) == f.end()) o += (o.empty() ? "" : "; ") + x;
		if (!o.empty()) fb_log_ += "[cpu] fallback off (the CPU keeps up again): " + o + "\n";
		fb_ = f; version_++;
		return true;
	}
	const std::vector<std::string> &fallbacks() const { return fb_; }
	// The sound card's measured sample-clock error (ppm, + = fast; self-calibration, src/modem/signum/
	// autocal.hpp, or the system-clock estimate). PACTOR's timing wants the card near nominal: the
	// emulated pair carried 130 ppm, but links failed early at 250-300 ppm, so warn above
	// kClockWarnPpm (200: some margin below the failures; the owner's note said ~300). The raw error
	// is warned about even though Signum then corrects it: a card this far off drifts with temperature
	// and is a bad sign. Logged once per change (cleared below kClockClearPpm, re-logged when the figure
	// moves by kClockStepPpm); listed by the console and the panel. Returns true: changed (publish again).
	static constexpr double kClockWarnPpm = 200.0, kClockClearPpm = 180.0, kClockStepPpm = 50.0;
	bool set_card_ppm(double ppm)
	{
		const double a = std::fabs(ppm);
		if (ppm_warn_ == 0.0 ? a <= kClockWarnPpm : (a < kClockClearPpm || a <= kClockWarnPpm || std::fabs(ppm - ppm_warn_) < kClockStepPpm)) {
			if (ppm_warn_ != 0.0 && a < kClockClearPpm) { ppm_warn_ = 0; fb_log_ += "[clock] sound card clock back within " + std::to_string(int(kClockClearPpm)) + " ppm\n"; version_++; return true; }
			return false;
		}
		ppm_warn_ = ppm;
		fb_log_ += "[clock] WARNING: " + clock_text() + "\n";
		version_++;
		return true;
	}
	double card_ppm_warned() const { return ppm_warn_; }
	std::string clock_text() const
	{
		if (ppm_warn_ == 0.0) return "";
		char b[260];
		std::snprintf(b, sizeof b, "the sound card's clock is %+.0f ppm off (more than %.0f): PACTOR timing may fail. Try another sound card or USB port (or a different USB audio adapter); Signum corrects the measured error (--audio-ppm sets it by hand) but a card this far off drifts",
		              ppm_warn_, kClockWarnPpm);
		return b;
	}
	bool no_realtime() const { return no_rt_; }
	bool clock_unpinned() const { return clock_; }

	static double weight(Kind k)
	{
		switch (k) {
		case UNDERRUN: return 2.0;
		case STALL: return 3.0;
		case LATE: return 1.5;
		case THIN: return 0.5;
		case BACKOFF: return 3.0;
		case SL1: return 3.0;
		case TXLATE: return 2.0;
		default: return 0;
		}
	}
	// One event at time t (seconds, any steady clock, the same one update() gets). ms: the size of it
	// (underrun: the period's work; stall: capture lost; thin: the margin; tx late: the wait).
	void event(Kind k, double t, double ms = 0)
	{
		if (t0_ < 0) t0_ = t;
		if (t - t0_ < kGraceS) { ignored_++; return; }
		ring_.push_back({t, k, ms});
		if (ring_.size() > 512) ring_.pop_front();
		total_[k]++;
		dirty_ = true;
	}
	// Called once per audio period (cheap: it works at most every 0.25 s unless an event came).
	// True when the state changed or, while not OK, the reason text did: publish again.
	bool update(double t)
	{
		if (t0_ < 0) t0_ = t;
		if (!dirty_ && t - last_eval_ < 0.25) return false;
		dirty_ = false; last_eval_ = t;
		while (!ring_.empty() && t - ring_.front().t > kWindowS) ring_.pop_front();
		const double sc = score(t);
		const int n = int(ring_.size());
		State want = OK;
		if (sc >= kStarvedScore && n >= kStarvedCount) want = STARVED;
		else if (sc >= kBusyScore && n >= kBusyCount) want = BUSY;
		// hysteresis: a state is held until the score has fallen to half its entry value
		if (state_ == STARVED && want < STARVED && sc >= kStarvedScore / 2 && n >= kBusyCount) want = STARVED;
		if (state_ >= BUSY && want == OK && sc >= kBusyScore / 2 && n >= 1) want = BUSY;
		bool changed = false;
		if (want > state_) { set_state(want, t); changed = true; }
		else if (want < state_ && t - since_ >= kMinDwellS) { set_state(want, t); changed = true; }
		if (state_ != OK) {
			const std::string r = reason(t);
			if (r != reason_shown_ && t - reason_t_ >= 1.0) { reason_shown_ = r; reason_t_ = t; changed = true; }
		} else reason_shown_.clear();
		if (changed) version_++;
		return changed;
	}
	State state() const { return state_; }
	unsigned version() const { return version_; }
	double since() const { return since_; }
	long total(Kind k) const { return total_[k]; }
	long ignored() const { return ignored_; }
	static const char *name(State s) { return s == OK ? "ok" : s == BUSY ? "busy" : "starved"; }
	static const char *upper(State s) { return s == OK ? "OK" : s == BUSY ? "BUSY" : "STARVED"; }

	// What is late, in the last minute: "2 playback underruns (worst: a period's work took 41 ms), 3 late decodes".
	std::string reason(double t) const
	{
		long cnt[NKINDS] = {0}; double worst[NKINDS] = {0}; double low_thin = INFINITY;
		for (const auto &e : ring_) {
			if (t - e.t > kWindowS) continue;
			cnt[e.k]++;
			if (e.k == THIN) low_thin = std::min(low_thin, e.ms); else worst[e.k] = std::max(worst[e.k], e.ms);
		}
		std::string r;
		auto add = [&](const std::string &s) { if (!r.empty()) r += ", "; r += s; };
		auto pl = [](long n, const char *one, const char *many) { return std::to_string(n) + " " + (n == 1 ? one : many); };
		if (cnt[UNDERRUN]) add(pl(cnt[UNDERRUN], "playback underrun", "playback underruns") + " (worst: a period's work took " + ms_str(worst[UNDERRUN]) + ")");
		if (cnt[STALL]) add(pl(cnt[STALL], "audio stall", "audio stalls") + " (worst: " + ms_str(worst[STALL]) + " of capture lost)");
		if (cnt[LATE]) add(pl(cnt[LATE], "late decode", "late decodes"));
		if (cnt[THIN]) add(pl(cnt[THIN], "decode", "decodes") + " with a thin margin (down to " + ms_str(low_thin) + ")");
		if (cnt[BACKOFF]) add(pl(cnt[BACKOFF], "computer back-off step", "computer back-off steps"));
		if (cnt[SL1]) add(pl(cnt[SL1], "packet", "packets") + " rescued from the SL1 decode");
		if (cnt[TXLATE]) add(pl(cnt[TXLATE], "transmit burst", "transmit bursts") + " late (waited up to " + ms_str(worst[TXLATE]) + ")");
		return r.empty() ? r : r + " in the last minute";
	}
	// The static conditions that apply here, as phrases.
	std::vector<std::string> applies() const
	{
		std::vector<std::string> a;
		if (no_rt_) a.push_back("no real-time priority");
		if (clock_) a.push_back(clock_why_.empty() ? "the CPU clock is not held up" : clock_why_);
		if (cores_ > 0 && load1_ > double(cores_)) {
			char b[96]; std::snprintf(b, sizeof b, "other load (load average %.1f on %d core%s)", load1_, cores_, cores_ == 1 ? "" : "s");
			a.push_back(b);
		}
		return a;
	}
	// What to do about it (only the parts that apply, then the general ones).
	std::string advice() const
	{
		std::string s = "Stop other programs that use the CPU (top shows which)";
		if (no_rt_) s += "; start Signum with real-time priority (make rt: setcap cap_sys_nice,cap_ipc_lock+ep, or an rtprio limit for the user)";
		if (clock_) s += "; let Signum hold the CPU clock up (the signum-cpufreq.service unit) or set the governor to performance";
		s += "; or move to a faster computer";
		return s;
	}
	// One log line for a change of state (empty when nothing new to say); take once.
	std::string take_log() { std::string l; l.swap(log_); l += fb_log_; fb_log_.clear(); return l; }
	// The console's answer.
	std::string console(double t) const
	{
		std::string s = std::string("*** CPU LOAD: ") + upper(state_);
		const std::string r = reason(t);
		if (!r.empty()) s += "  (" + r + ")";
		const auto ap = applies();
		if (!ap.empty()) { s += "  APPLIES HERE: "; for (size_t i = 0; i < ap.size(); i++) s += (i ? "; " : "") + ap[i]; }
		if (!fb_.empty()) { s += "  FALLBACKS: "; for (size_t i = 0; i < fb_.size(); i++) s += (i ? "; " : "") + fb_[i]; }
		if (ppm_warn_ != 0.0) s += "  *** SOUND CARD CLOCK: " + clock_text();
		return s;
	}
	// The panel's "cpu" object: {"state":"ok|busy|starved","reason":..,"applies":[..],"advice":..,"counts":{..}}
	std::string json(double t) const
	{
		long cnt[NKINDS] = {0};
		for (const auto &e : ring_) if (t - e.t <= kWindowS) cnt[e.k]++;
		std::string j = std::string("{\"state\":\"") + name(state_) + "\",\"level\":" + std::to_string(int(state_)) +
		                ",\"reason\":" + quote(state_ == OK ? "" : reason(t)) + ",\"applies\":[";
		const auto ap = applies();
		for (size_t i = 0; i < ap.size(); i++) j += (i ? "," : "") + quote(ap[i]);
		j += "],\"fallbacks\":[";
		for (size_t i = 0; i < fb_.size(); i++) j += (i ? "," : "") + quote(fb_[i]);
		if (ppm_warn_ != 0.0) { char b[80]; std::snprintf(b, sizeof b, "sound card clock %+.0f ppm off", ppm_warn_); j += std::string(fb_.empty() ? "" : ",") + quote(b); }   // listed with the notices the panel already shows
		j += "],\"clock_warning\":" + quote(clock_text()) + ",\"advice\":" + quote(state_ == OK ? "" : advice()) + ",\"counts\":{";
		static const char *const nm[NKINDS] = {"underrun", "stall", "late", "thin", "backoff", "sl1", "tx_late"};
		for (int k = 0; k < NKINDS; k++) j += std::string(k ? "," : "") + "\"" + nm[k] + "\":" + std::to_string(cnt[k]);
		return j + "}}";
	}

private:
	struct Ev { double t; Kind k; double ms; };
	double score(double t) const
	{
		double s = 0;
		for (const auto &e : ring_) s += weight(e.k) * std::exp(-(t - e.t) / kTauS);
		return s;
	}
	static std::string ms_str(double v) { char b[32]; std::snprintf(b, sizeof b, v >= 10 ? "%.0f ms" : "%.1f ms", v); return b; }
	static std::string quote(const std::string &s)
	{
		std::string o = "\"";
		for (unsigned char c : s) {
			if (c == '"' || c == '\\') { o += '\\'; o += char(c); }
			else if (c < 0x20) o += ' ';
			else o += char(c);
		}
		return o + "\"";
	}
	void set_state(State w, double t)
	{
		const State old = state_;
		state_ = w; since_ = t;
		if (w == OK) {
			log_ = "[cpu] OK again: nothing late for a while (was " + std::string(upper(old)) + ")\n";
			return;
		}
		std::string l = std::string("[cpu] ") + upper(w) + ": " + reason(t) + ". ";
		if (w == STARVED) l += "This computer is not keeping up with the audio and the decodes: links may fall back to low speed levels or drop. ";
		const auto ap = applies();
		if (ap.empty()) l += "Nothing known applies (real-time priority is on, the clock is held and the load average is not above the core count): the computer itself is too slow. ";
		else { l += "Applies here: "; for (size_t i = 0; i < ap.size(); i++) l += (i ? "; " : "") + ap[i]; l += ". "; }
		l += "What to do: " + advice() + ".\n";
		log_ = l;
	}

	State state_ = OK;
	double since_ = 0, t0_ = -1, last_eval_ = -1, reason_t_ = -1;
	bool dirty_ = false, no_rt_ = false, clock_ = false;
	std::string clock_why_, reason_shown_, log_, fb_log_;
	std::vector<std::string> fb_;
	double load1_ = 0, ppm_warn_ = 0; int cores_ = 0;
	unsigned version_ = 0;
	long total_[NKINDS] = {0}, ignored_ = 0;
	std::deque<Ev> ring_;
};

}  // namespace signum

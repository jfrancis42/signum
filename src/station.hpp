// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// station.hpp: the modem (the modem's P2Station) behind the station parameters.
//
// P2Station takes most of its parameters only at construction (P2Config).
// The terminal commands that change one of those (MAXErr, TXDelay, PSKAmpl, and
// the call timing) set it here; it applies at once while the modem is idle
// (the station is built again from the new P2Config: nothing of a link is
// lost, there is none), else as soon as the link has ended. Parameters with
// a setter (MYcall, MYLevel, TOnes, MOde, the greeting) go straight through.
//
// The station parameters without a counterpart in the modem are kept and
// reported (MAXUp, MAXDown, MAXTry, MAXSum, FSKAmpl, CSDelay, ...): the modem
// follows the firmware's own defaults for its speed changes (docs/md/commands.md).
//
// Portable: no OS calls (the ESP32 rule).
#pragma once
#include <cmath>
#include <memory>
#include <string>

#include "signum/p2_station.hpp"

namespace signum {

struct HostParams {
	int maxerr = 70;        // MAXErr 30..255 (255 = no timeout): P2Config::timeout_cycles
	int txdelay = 4;        // TXDelay 1..31, x 5 ms: P2Config::txd_s
	int pska = 140;         // PSKAmpl 10..1000 mV: scales P2Config::amplitude (4400 at 140, the firmware's level)
	int fska = 60;          // FSKAmpl 10..1000 mV: kept (the modem's FSK level is a fixed ratio of the PSK level)
	int maxup = 4, maxdown = 5, maxtry = 2, maxsum = 30;   // kept: the modem uses the firmware's defaults (firmware: MAXDown 5, the manual says 6)
	int csdelay = 5;        // CSDelay: kept (firmware default 5)
	int chob = 1, bkchr = 25, chochr = 25, qrtchr = 4, escchr = 27;
	int listen = 1, status = 1, hostch = 4, cwid = 0, contype = 0, rem = 0;
	float tx_gain_db = 0;   // --tx-gain (the command line): on top of PSKAmpl
};

class Station {
public:
	explicit Station(const fmodem::P2Config &c) : cfg_(c) { build(); }

	fmodem::P2Station &st() { return *st_; }
	const fmodem::P2Station &st() const { return *st_; }
	const fmodem::P2Config &config() const { return cfg_; }
	HostParams &params() { return p_; }
	const HostParams &params() const { return p_; }

	// A parameter that needs a new P2Station changed: applied now when idle, else after the link.
	void params_changed()
	{
		fmodem::P2Config c = cfg_;
		c.timeout_cycles = p_.maxerr >= 255 ? 1 << 30 : p_.maxerr;
		c.txd_s = 0.005f * float(p_.txdelay);
		c.amplitude = 4400.0f * float(p_.pska) / 140.0f * std::pow(10.0f, p_.tx_gain_db / 20.0f);
		pending_ = c;
		have_pending_ = true;
		tick_idle();
	}
	// From the loop, between samples: apply what is pending when the modem is idle.
	void tick_idle()
	{
		if (!have_pending_ || !quiet()) return;
		// what the station's setters changed since it was built
		pending_.mycall = st_->mycall();
		pending_.tone_lo = st_->tone_lo(); pending_.tone_hi = st_->tone_hi();
		pending_.allow_p3 = st_->allow_p3(); pending_.p1_only = st_->p1_only();
		pending_.compress = st_->compress();
		pending_.allow_pmc = st_->tx_mode() >= 2;
		pending_.rt_latency_s = st_->rt_latency(); pending_.reply_adjust_s = st_->reply_adjust();
		pending_.p3_greeting = cfg_.p3_greeting;
		pending_.p3_request_long = st_->p3_long();
		pending_.pduplex = st_->pduplex(); pending_.pdtimer_s = st_->pdtimer();
		const int ceil = st_->p3_long_ceiling();       // what this computer's long decodes were measured to manage
		cfg_ = pending_;
		have_pending_ = false;
		build();
		st_->set_p3_long_ceiling(ceil);
		rebuilt_++;
	}
	int rebuilt() const { return rebuilt_; }
	// No link, no call, no unproto (P2Station's idle() is false while it listens for calls).
	bool quiet() const { return !st_->link_up() && !st_->setting_up() && !st_->unproto(); }
	// The tones in the station's own config too (a rebuild keeps them).
	void set_tones(float lo, float hi) { st_->set_tones(lo, hi); cfg_.tone_lo = lo; cfg_.tone_hi = hi; }
	void set_greeting(const std::string &g) { st_->set_greeting(g); cfg_.p3_greeting = g; }
	// Adaptive CPU features (CPUadapt): on = adaptive (long packets as P3 receiver, dropped by level when
	// this computer's decodes cannot keep up), off = never asked for.
	void set_cpu_adaptive(bool on) { st_->set_p3_long(on); cfg_.p3_request_long = on; }
	bool cpu_adaptive() const { return st_->p3_long(); }
	// PDuplex / PDTimer (break-in as receiver and "over" as sender, owner decision a): live, kept across a rebuild.
	void set_pduplex(bool on, int timer_s) { st_->set_pduplex(on, timer_s); cfg_.pduplex = on; cfg_.pdtimer_s = timer_s; }

private:
	void build()
	{
		st_.reset();                                  // the old one first (the ESP32's heap)
		st_.reset(new fmodem::P2Station(cfg_));
	}
	fmodem::P2Config cfg_, pending_;
	bool have_pending_ = false;
	int rebuilt_ = 0;
	HostParams p_;
	std::unique_ptr<fmodem::P2Station> st_;
};

}  // namespace signum

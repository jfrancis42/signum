// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// monitor_thread.hpp: listen mode (LISTEN 1, TODO 12): the monitor
// (src/modem/signum/monitor.hpp) on its own thread, fed from the station's
// audio loop through a single-producer ring. The loop only copies samples:
// a slow decode never delays the station; if the monitor falls behind the
// ring overwrites and the monitor loses its place, never the station.
#pragma once
#include <pthread.h>
#include <sched.h>
#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "signum/monitor.hpp"
#include "signum/p2_unproto_rx.hpp"

class MonitorThread {
public:
	struct Line { double t; std::string who, text; };
	~MonitorThread() { stop(); }
	void start(const fmodem::P2Config &cfg)
	{
		if (run_) return;
		cfg_ = cfg; run_ = true; w_ = r_ = 0; reset_ = true;
		th_ = std::thread([this] { loop(); });
	}
	void stop() { if (!run_) return; run_ = false; if (th_.joinable()) th_.join(); }
	bool running() const { return run_; }
	// From the audio loop: samples heard (9600 Hz), or a gap (our own link: the monitor starts afresh).
	void push(const int16_t *x, size_t n)
	{
		size_t w = w_.load(std::memory_order_relaxed);
		for (size_t i = 0; i < n; i++) ring_[(w + i) % kRing] = x[i];
		w_.store(w + n, std::memory_order_release);
	}
	void reset() { reset_ = true; }
	std::deque<Line> take() { std::lock_guard<std::mutex> g(m_); std::deque<Line> l; l.swap(out_); return l; }

private:
	static constexpr size_t kRing = 9600 * 20;          // 20 s
	int16_t ring_[kRing];
	std::atomic<size_t> w_{0}, r_{0};
	std::atomic<bool> run_{false}, reset_{true};
	std::thread th_;
	std::mutex m_;
	std::deque<Line> out_;
	fmodem::P2Config cfg_;
	void loop()
	{
		// Normal priority, never the audio loop's SCHED_FIFO 80 (inherited: start() is called from the audio
		// loop), and so are the decode pools of the monitor's receivers: idle work, below every real-time
		// thread (it took ~20 % of a Raspberry Pi 4 core at FIFO 80 before, 2026-11-16).
		{ sched_param sp{}; pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp); }
		fmodem::par::background_thread() = true;
		std::unique_ptr<fmodem::P2Monitor> mon;
		// unproto broadcasts too (CQ calls; P1, P2 short and long cycle: fixes 138/140/142), beside the
		// QSOs, one receiver owning the output at a time as the reference unit's standby prints it
		// (fix 239: run side by side, the three printed false characters into a real unit's U 3)
		// (made afresh with the monitor: after a jump in the audio its grids and timings are wrong; P3 unproto
		// decodes can take long enough on a small computer for the ring to overrun)
		auto mk_up = [&]() { return std::unique_ptr<fmodem::AllUnprotoRx>(new fmodem::AllUnprotoRx(fmodem::p2arq::kLongHeader, int(cfg_.fs), cfg_.tone_lo, cfg_.tone_hi)); };
		auto up = mk_up();
		bool mon_held = false;
		uint64_t up_n = 0;
		while (run_) {
			if (reset_.exchange(false) || !mon) { mon.reset(new fmodem::P2Monitor(cfg_)); up = mk_up(); r_.store(w_.load(std::memory_order_acquire)); }
			const size_t w = w_.load(std::memory_order_acquire);
			size_t r = r_.load();
			if (w - r > kRing - 9600) { r = w - kRing / 2; mon.reset(new fmodem::P2Monitor(cfg_)); up = mk_up(); }   // fell behind: start afresh
			if (r == w) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); continue; }
			// while a P3 unproto transmission is held the QSO monitor pauses (nothing else is on the channel; on P3
			// SL1 bursts it costs more than real time: U 36, 60 s of audio, 83 s on a workstation), then starts afresh
			for (; r != w && run_; r++) {
				if (up->p3_holds()) mon_held = true;
				else { if (mon_held) { mon.reset(new fmodem::P2Monitor(cfg_)); mon_held = false; } mon->tick(ring_[r % kRing]); }
				up->tick(ring_[r % kRing]); up_n++;
			}
			r_.store(r);
			auto lines = mon->take();
			if (std::string u = up->take(); !u.empty()) { std::lock_guard<std::mutex> g(m_); out_.push_back({double(up_n) / cfg_.fs, "UNPROTO", u}); }
			if (!lines.empty()) {
				std::lock_guard<std::mutex> g(m_);
				for (const auto &l : lines) out_.push_back({l.t, l.who < 0 ? std::string() : mon->call(l.who), l.text});
			}
		}
	}
};

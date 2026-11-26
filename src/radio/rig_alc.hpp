// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// rig_alc.hpp: the radio's ALC meter through rigctld, for the ALC warning (src/alc_warn.hpp, --alc-warn).
// The AlcReader of the program. Nothing here keys, sets or changes anything on the radio: it only reads
// ("l ALC", Hamlib's 0..1 level).
//
// A worker thread with a connection of its own to the PTT's rigctld (--ptt rig[:HOST[:PORT]]); the audio loop
// never waits on it: keyed() sets two atomics, drain() takes the reading queue with try_lock.
//
//   while the transmitter is keyed: after kFirstS (the audio has not started at the key-down: the meter
//   would read 0) one "l ALC" every kEveryS; the connection is made at the first key-down and kept.
//   An answer "RPRT n" is an error: n = -1, -4 or -11 (invalid parameter, not implemented, not available)
//   means this radio gives no ALC reading and ends the polling for this run (state 2); any other error
//   counts, three in a row end it too. No rigctld at HOST:PORT: state 2 for now, tried again at the next key-down.
//
// Cost [inferred]: rigctld serves one command at a time on the radio's CI-V line, and a read takes tens of
// milliseconds at 19200 baud, so a key-down or key-up sent while a read is under way can be late by about
// that. The poll is every 0.25 s and only while keyed; --alc-warn off stops it.
#pragma once

#include "rigctl.hpp"          // tcp_connect
#include "../alc_warn.hpp"

#include <sys/socket.h>
#include <poll.h>

#include <atomic>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace radio {

class RigAlc : public signum::AlcReader {
public:
	static constexpr int kFirstMs = 300, kEveryMs = 250;
	RigAlc(const std::string &host, int port, int first_ms = kFirstMs, int every_ms = kEveryMs)
	    : host_(host.empty() ? "127.0.0.1" : host), port_(port ? port : 4532), first_ms_(first_ms), every_ms_(every_ms)
	{
		th_ = std::thread([this] { run(); });
	}
	~RigAlc() override
	{
		quit_ = true;
		if (th_.joinable()) th_.join();
		if (fd_ >= 0) ::close(fd_);
	}
	void keyed(bool k, unsigned burst) override { if (k) burst_ = burst; keyed_ = k; }
	bool drain(std::vector<signum::AlcReading> &out) override
	{
		std::unique_lock<std::mutex> g(m_, std::try_to_lock);
		if (!g.owns_lock() || q_.empty()) return false;
		out.swap(q_);
		return true;
	}
	int state(std::string &why) override
	{
		const int s = state_.load();
		if (s == 2) { std::lock_guard<std::mutex> g(why_m_); why = why_; }
		return s;
	}

private:
	std::string host_;
	int port_, first_ms_, every_ms_;
	std::thread th_;
	std::atomic<bool> quit_{false}, keyed_{false};
	std::atomic<unsigned> burst_{0};
	std::atomic<int> state_{0};
	std::mutex m_, why_m_;
	std::vector<signum::AlcReading> q_;
	std::string why_;
	int fd_ = -1;                              // the worker's
	std::string rbuf_;
	int errs_ = 0;

	void set_state(int s, const std::string &why = "")
	{
		if (!why.empty()) { std::lock_guard<std::mutex> g(why_m_); why_ = why; }
		state_ = s;
	}
	void drop() { if (fd_ >= 0) ::close(fd_); fd_ = -1; rbuf_.clear(); }
	bool read_line(std::string &l, int ms)
	{
		const auto t0 = std::chrono::steady_clock::now();
		for (;;) {
			const size_t p = rbuf_.find('\n');
			if (p != std::string::npos) { l = rbuf_.substr(0, p); rbuf_.erase(0, p + 1); if (!l.empty() && l.back() == '\r') l.pop_back(); return true; }
			const int left = ms - int(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
			if (left <= 0) return false;
			pollfd pf{fd_, POLLIN, 0};
			if (::poll(&pf, 1, left) != 1) return false;
			char b[256];
			const ssize_t n = ::recv(fd_, b, sizeof b, 0);
			if (n <= 0) return false;
			rbuf_.append(b, size_t(n));
		}
	}
	bool nap(int ms, unsigned id)               // false: the burst ended (or the program is stopping)
	{
		for (int i = 0; i < ms / 10; i++) {
			if (quit_ || !keyed_ || burst_ != id) return false;
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		return !quit_ && keyed_ && burst_ == id;
	}
	// One read. 1 a value, 0 an error answer (errs_ counted), -1 no answer (connection dropped)
	int read_alc(double &v)
	{
		rbuf_.clear();
		const std::string w = "l ALC\n";
		std::string a;
		if (::send(fd_, w.data(), w.size(), MSG_NOSIGNAL) != ssize_t(w.size()) || !read_line(a, 1000)) { drop(); return -1; }
		if (a.compare(0, 5, "RPRT ") == 0) {
			const int n = std::atoi(a.c_str() + 5);
			errs_++;
			if (n == -1 || n == -4 || n == -11 || errs_ >= 3) set_state(2, "the radio gives no ALC reading (rigctld answered \"" + a + "\" to l ALC)");
			return 0;
		}
		char *e = nullptr;
		v = std::strtod(a.c_str(), &e);
		if (e == a.c_str() || !std::isfinite(v)) { errs_++; if (errs_ >= 3) set_state(2, "rigctld's answer to l ALC is not a number (\"" + a + "\")"); return 0; }
		errs_ = 0;
		return 1;
	}
	void run()
	{
		while (!quit_) {
			if (!keyed_) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); continue; }
			const unsigned id = burst_;
			if (state_ == 2 && fd_ >= 0) { nap(1 << 20, id); continue; }   // the radio gives no reading: nothing more to ask this run
			if (!nap(first_ms_, id)) continue;
			if (fd_ < 0) {
				fd_ = tcp_connect(host_, port_, 1500);
				if (fd_ < 0) { set_state(2, "no rigctld at " + host_ + ":" + std::to_string(port_)); nap(1 << 20, id); continue; }
				rbuf_.clear(); errs_ = 0;
				set_state(0);
			}
			if (errs_ >= 3 || (state_ == 2)) { nap(1 << 20, id); continue; }
			do {
				double v = 0;
				const int r = read_alc(v);
				if (r == 1) {
					{ std::lock_guard<std::mutex> g(m_); q_.push_back({v < 0 ? 0 : v > 1.5 ? 1.5 : v, id}); }
					set_state(1);
				} else if (r == -1) { set_state(2, "rigctld at " + host_ + ":" + std::to_string(port_) + " did not answer l ALC"); break; }
				else if (state_ == 2) break;
			} while (nap(every_ms_, id));
			if (state_ == 2 && fd_ < 0) continue;
		}
	}
};

}  // namespace radio

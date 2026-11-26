// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// rig_tune.hpp: the radio's own antenna-tuner cycle through rigctld, for the tune before a new call
// (src/tune_tone.hpp, --tune-method). The RadioTuner adapter of the program; nothing here keys the
// transmitter: the radio does, at its own RF POWER setting.
//
// A connection of its own to the same rigctld the PTT uses (--ptt rig[:HOST[:PORT]]), kept between tunes,
// used only by a worker thread (the audio loop never waits on it):
//
//   1. connect, once per connection ask "\dump_caps" and keep whether "VFO Ops:" lists TUNE
//      (the answer is dropped with the connection: a rigctld restarted with another rig is asked again)
//   2. "t": the radio must read 0 (not transmitting), else no tune is started. A rig that cannot report its PTT
//      (rigctld answers RPRT -11, as Hamlib's dummy rig does) is tuned anyway and the full wait is used
//   3. "G TUNE": anything but RPRT 0 means this attempt is unsupported (nothing was started)
//   4. wait up to S seconds, "t" every 100 ms: once the radio has read keyed and then read 0 for
//      300 ms, it is back in receive and the wait ends early. A radio that never reports the tune as PTT
//      (the IC-7300 may not) is waited for the full S seconds.
//
// Result to the caller (poll): done (with what was seen) or failed BEFORE the radio was told to tune (the
// caller sends the tone instead). A problem after "G TUNE" was accepted is not a failure: the radio is
// tuning, so the wait is finished (or kept) and the call follows.
// An abort (cancel) stops the wait; the radio is never told to stop.
#pragma once

#include "rigctl.hpp"          // tcp_connect
#include "../tune_tone.hpp"

#include <sys/socket.h>
#include <poll.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace radio {

class RigTune : public signum::RadioTuner {
public:
	RigTune(const std::string &host, int port) : host_(host.empty() ? "127.0.0.1" : host), port_(port ? port : 4532) {}
	~RigTune() override
	{
		std::vector<std::pair<std::thread, std::shared_ptr<Job>>> j;
		{ std::lock_guard<std::mutex> g(m_); j.swap(jobs_); }
		for (auto &x : j) x.second->abort = true;
		for (auto &x : j) if (x.first.joinable()) x.first.join();
		if (fd_ >= 0) ::close(fd_);
	}
	// Whether this radio lists TUNE is learnt on the connection (by the worker): only what is already known is said here.
	bool available(std::string &info) override
	{
		info = caps_.load() == 1 ? "TUNE supported" : "";
		return true;
	}
	void start(int secs) override
	{
		auto job = std::make_shared<Job>();
		job->secs = secs;
		std::lock_guard<std::mutex> g(m_);
		if (cur_) cur_->abort = true;
		for (size_t i = 0; i < jobs_.size();)            // the finished ones, joined now
			if (jobs_[i].second->finished) { if (jobs_[i].first.joinable()) jobs_[i].first.join(); jobs_.erase(jobs_.begin() + long(i)); }
			else i++;
		cur_ = job;
		jobs_.emplace_back(std::thread([this, job] { run(*job); job->finished = true; }), job);
	}
	int poll(std::string &text) override
	{
		std::shared_ptr<Job> j; { std::lock_guard<std::mutex> g(m_); j = cur_; }
		if (!j) { text = "no tune started"; return 2; }
		const int s = j->state.load();
		if (s == 0) return 0;
		std::lock_guard<std::mutex> g(j->m);
		text = j->text;
		return s == 1 ? 1 : 2;
	}
	void cancel() override { std::lock_guard<std::mutex> g(m_); if (cur_) cur_->abort = true; }
	// 1 the radio's VFO ops list TUNE, 0 they do not, -1 not known (no connection yet)
	int tune_known() const { return caps_.load(); }

	// "VFO Ops:" in rigctld's \dump_caps text, and whether TUNE is among them (a whole word)
	static bool caps_have_tune(const std::vector<std::string> &lines)
	{
		for (const auto &l : lines) {
			if (l.compare(0, 8, "VFO Ops:") != 0) continue;
			size_t p = 8;
			while (p < l.size()) {
				while (p < l.size() && (l[p] == ' ' || l[p] == '\t')) p++;
				size_t e = p;
				while (e < l.size() && l[e] != ' ' && l[e] != '\t') e++;
				if (l.compare(p, e - p, "TUNE") == 0 && e - p == 4) return true;
				p = e;
			}
		}
		return false;
	}

private:
	struct Job {
		int secs = 0;
		std::atomic<int> state{0};            // 0 running, 1 done, 2 failed
		std::atomic<bool> abort{false}, finished{false};
		std::mutex m;
		std::string text;
	};
	std::string host_;
	int port_;
	std::mutex m_, conn_m_;                   // m_: the job list; conn_m_: one worker at a time on the connection
	std::vector<std::pair<std::thread, std::shared_ptr<Job>>> jobs_;
	std::shared_ptr<Job> cur_;
	std::atomic<int> caps_{-1};
	int fd_ = -1;                             // the worker's (under conn_m_)
	std::string rbuf_;

	static void finish(Job &j, int state, const std::string &t) { { std::lock_guard<std::mutex> g(j.m); j.text = t; } j.state = state; }
	void drop() { if (fd_ >= 0) ::close(fd_); fd_ = -1; rbuf_.clear(); caps_ = -1; }
	bool send_line(const std::string &c)
	{
		const std::string w = c + "\n";
		return ::send(fd_, w.data(), w.size(), MSG_NOSIGNAL) == ssize_t(w.size());
	}
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
			char b[1024];
			const ssize_t n = ::recv(fd_, b, sizeof b, 0);
			if (n <= 0) return false;
			rbuf_.append(b, size_t(n));
		}
	}
	// One command with a one-line answer ("t": "0"/"1"; a set: "RPRT n"). False: no answer (the connection is dropped by the caller).
	bool ask(const std::string &c, std::string &ans, int ms)
	{
		rbuf_.clear();
		return send_line(c) && read_line(ans, ms);
	}
	// "+\dump_caps": every line up to RPRT.
	bool dump_caps(std::vector<std::string> &L)
	{
		rbuf_.clear();
		if (!send_line("+\\dump_caps")) return false;
		for (;;) {
			std::string l;
			if (!read_line(l, 4000)) return false;
			if (l.compare(0, 5, "RPRT ") == 0) return std::atoi(l.c_str() + 5) == 0;
			L.push_back(l);
		}
	}
	bool pause(Job &j, int ms)                 // false: aborted
	{
		for (int i = 0; i < ms / 20 && !j.abort; i++) std::this_thread::sleep_for(std::chrono::milliseconds(20));
		return !j.abort;
	}
	void run(Job &j)
	{
		std::lock_guard<std::mutex> cg(conn_m_);
		const std::string where = host_ + ":" + std::to_string(port_);
		std::string ans, no_ptt;
		for (int attempt = 0;; attempt++) {
			if (j.abort) { finish(j, 1, "aborted"); return; }
			if (fd_ < 0) {                      // a connection, and what the radio can do on it
				fd_ = tcp_connect(host_, port_, 1500);
				if (fd_ < 0) { finish(j, 2, "no rigctld at " + where); return; }
				rbuf_.clear(); caps_ = -1;
				std::vector<std::string> L;
				if (!dump_caps(L)) { drop(); if (attempt == 0) continue; finish(j, 2, "rigctld at " + where + " did not answer \\dump_caps"); return; }
				caps_ = caps_have_tune(L) ? 1 : 0;
			}
			if (caps_ == 0) { finish(j, 2, "this radio's VFO ops (rigctld \\dump_caps) do not list TUNE"); return; }
			if (!ask("t", ans, 2000)) { drop(); if (attempt == 0) continue; finish(j, 2, "rigctld at " + where + " did not answer"); return; }   // a stale connection: once more on a new one
			if (ans.compare(0, 5, "RPRT ") == 0) { no_ptt = ans; break; }   // the rig cannot report its PTT: the tune goes ahead, without the early end
			if (ans != "0") { finish(j, 2, ans == "1" ? "the radio is already transmitting" : "rigctld answered \"" + ans + "\" to t, the radio's PTT"); return; }
			break;
		}
		if (j.abort) { finish(j, 1, "aborted"); return; }
		if (!ask("G TUNE", ans, 3000)) { drop(); finish(j, 2, "rigctld did not answer G TUNE"); return; }
		if (ans != "RPRT 0") { finish(j, 2, "G TUNE refused: " + ans); return; }   // nothing was started
		const auto t0 = std::chrono::steady_clock::now();
		auto el = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
		bool keyed = false, lost = false;
		double key_t = 0;
		int zeros = 0;
		char b[160];
		while (el() < j.secs) {
			if (!pause(j, 100)) { finish(j, 1, "aborted"); return; }
			if (!no_ptt.empty()) continue;           // nothing to read: the full wait
			if (!ask("t", ans, 1000)) { drop(); lost = true; break; }
			if (ans.compare(0, 5, "RPRT ") == 0) { no_ptt = ans; continue; }
			if (ans == "1") { if (!keyed) key_t = el(); keyed = true; zeros = 0; }
			else if (ans == "0" && keyed && ++zeros >= 3) {
				std::snprintf(b, sizeof b, "TUNE supported; the radio keyed for %.1f s and is back in receive", el() - key_t - 0.2);
				finish(j, 1, b);
				return;
			}
		}
		if (lost) {                              // rigctld gone after the radio was told to tune: wait the rest, then the call follows
			while (el() < j.secs) if (!pause(j, 100)) { finish(j, 1, "aborted"); return; }
			std::snprintf(b, sizeof b, "TUNE accepted; lost rigctld during the wait, waited the full %d s", j.secs);
		} else if (!no_ptt.empty()) std::snprintf(b, sizeof b, "TUNE supported; this rig cannot report PTT (%s), waited the full %d s", no_ptt.c_str(), j.secs);
		else if (keyed) std::snprintf(b, sizeof b, "TUNE supported; the radio was still transmitting after %d s", j.secs);
		else std::snprintf(b, sizeof b, "TUNE supported; waited the full %d s, the radio never reported the tune as PTT", j.secs);
		finish(j, 1, b);
	}
};

}  // namespace radio

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// async_out.hpp: output written by a background thread, so the real-time
// audio loop never blocks on a file (2026-10-18: a log fflush stalled the
// loop 71 ms once on the loopback bench, an underrun at a 25 ms prime).
// The audio thread appends to private per-stream buffers (put) and hands
// them over once per period (commit: try_lock, never waits; if the writer
// holds the lock the data simply goes with the next period). A normal-
// priority thread writes and flushes every 20 ms; everything left is written
// at exit (finish, also from atexit).
#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <pthread.h>
#include <sched.h>
#include <string>
#include <thread>
#include <vector>

class AsyncOut {
public:
	// a joinable writer thread at exit() would std::terminate (SIGABRT): every path
	// that reaches exit(), e.g. a failed audio open, flushes and joins here
	~AsyncOut() { finish(); }
	int add(FILE *f)
	{
		streams_.push_back(Stream{f, {}, {}});
		return int(streams_.size() - 1);
	}
	void start() { th_ = std::thread([this] { run(); }); }
	// audio thread: append (no locking, no system calls)
	void put(int id, const void *p, size_t n) { streams_[size_t(id)].local.append(static_cast<const char *>(p), n); }
	void say(int id, const char *fmt, ...) __attribute__((format(printf, 3, 4)))
	{
		char b[1024];
		va_list ap; va_start(ap, fmt);
		const int k = std::vsnprintf(b, sizeof b, fmt, ap);
		va_end(ap);
		if (k > 0) put(id, b, size_t(k < int(sizeof b) ? k : int(sizeof b) - 1));
	}
	void say_str(int id, const std::string &s) { put(id, s.data(), s.size()); }
	// audio thread, once per period
	void commit()
	{
		bool any = false;
		for (auto &s : streams_) any = any || !s.local.empty();
		if (!any || !m_.try_lock()) return;
		for (auto &s : streams_) if (!s.local.empty()) { s.pend.append(s.local); s.local.clear(); }
		m_.unlock();
	}
	// at exit: stop the writer, then write whatever is left (in order)
	void finish()
	{
		if (done_) return;
		done_ = true;
		{ std::lock_guard<std::mutex> g(m_); stop_ = true; }
		cv_.notify_one();
		if (th_.joinable()) {
			if (th_.get_id() == std::this_thread::get_id()) th_.detach();   // exit() from a signal on the writer itself
			else th_.join();
		}
		for (auto &s : streams_) {
			s.pend.append(s.local); s.local.clear();
			if (!s.pend.empty() && s.f) { std::fwrite(s.pend.data(), 1, s.pend.size(), s.f); std::fflush(s.f); }
			s.pend.clear();
		}
	}
private:
	struct Stream { FILE *f; std::string local, pend; };
	std::vector<Stream> streams_;
	std::mutex m_;
	std::condition_variable cv_;
	std::thread th_;
	bool stop_ = false, done_ = false;
	void run()
	{
		sched_param sp{}; sp.sched_priority = 0;   // never the audio loop's SCHED_FIFO (inherited when started after it)
		pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp);
		std::vector<std::string> out(streams_.size());
		for (;;) {
			bool stopping;
			{
				std::unique_lock<std::mutex> g(m_);
				cv_.wait_for(g, std::chrono::milliseconds(20), [this] { return stop_; });
				stopping = stop_;
				for (size_t i = 0; i < streams_.size(); i++) { out[i].swap(streams_[i].pend); streams_[i].pend.clear(); }
			}
			for (size_t i = 0; i < streams_.size(); i++)
				if (!out[i].empty() && streams_[i].f) { std::fwrite(out[i].data(), 1, out[i].size(), streams_[i].f); std::fflush(streams_[i].f); out[i].clear(); }
			if (stopping) return;
		}
	}
};

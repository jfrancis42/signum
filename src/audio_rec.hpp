// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// AudioRec: a recording of what the modem heard and sent, for replaying a link off the air
// (fix 263, 2026-11-18: the P3 receive shift walked on the air and there was no audio to replay).
// SIGNUM_REC_AUDIO=FILE.wav[:MB] turns it on (off by default; "%p" in FILE: the process id): a WAV file, 2 channels at 9600 Hz,
// 16 bit: left the received audio exactly as the modem took it (after the resampler, the samples
// its decoders see; silence where a stalled capture was skipped), right what the modem sent. At
// most MB megabytes (default 200: ~43 minutes); then the recording stops and the station goes on.
// The audio loop only copies into a ring (10 s); a writer thread empties it every 0.2 s and keeps
// the WAV header's sizes current every second, so a file cut short (power, kill -9) still opens.
// A ring overrun (the disk slower than the audio, for 10 s) is counted and logged, never waited for.
// The same thread keeps a once-a-second picture of the computer (load average, iowait, the busiest
// processes over the last second, from /proc), which the station adds to its timing lines while
// recording (snapshot(): no wait on the audio loop, an empty string if the thread holds the lock).
#pragma once

#include <dirent.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace signum {

class AudioRec {
public:
	~AudioRec() { stop(); }
	// SIGNUM_REC_AUDIO's value; false (and why) if the file cannot be made.
	bool start(const std::string &spec, std::string &msg)
	{
		std::string path = spec;
		max_bytes_ = 200ull << 20;
		const size_t c = spec.rfind(':');
		if (c != std::string::npos && c + 1 < spec.size() && spec.find_first_not_of("0123456789", c + 1) == std::string::npos) {
			path = spec.substr(0, c);
			max_bytes_ = std::strtoull(spec.c_str() + c + 1, nullptr, 10) << 20;
		}
		if (max_bytes_ < (1ull << 20)) max_bytes_ = 1ull << 20;
		if (const size_t p = path.find("%p"); p != std::string::npos) path.replace(p, 2, std::to_string(long(getpid())));   // two stations, one environment
		f_ = std::fopen(path.c_str(), "wb");
		if (!f_) { msg = "cannot write " + path + ": " + std::strerror(errno); return false; }
		header(0);
		ring_.assign(kRing * 2, 0);
		run_ = true;
		th_ = std::thread([this] { writer(); });
		char b[256];
		std::snprintf(b, sizeof b, "recording what the modem hears (left) and sends (right), 9600 Hz, to %s, at most %llu MB (SIGNUM_REC_AUDIO)",
		              path.c_str(), (unsigned long long)(max_bytes_ >> 20));
		msg = b;
		return true;
	}
	bool on() const { return f_ != nullptr; }
	std::string snapshot()
	{
		std::unique_lock<std::mutex> l(snap_m_, std::try_to_lock);
		return l.owns_lock() ? snap_ : std::string();
	}
	// The audio loop: n frames (no lock, no wait; dropped and counted if the ring is full).
	void push(const int16_t *rx, const int16_t *tx, size_t n)
	{
		if (!f_ || full_.load(std::memory_order_relaxed)) return;
		const uint64_t w = wpos_.load(std::memory_order_relaxed), r = rpos_.load(std::memory_order_acquire);
		if (w - r + n > kRing) { dropped_.fetch_add(n, std::memory_order_relaxed); return; }
		for (size_t i = 0; i < n; i++) {
			const size_t k = size_t((w + i) % kRing) * 2;
			ring_[k] = rx ? rx[i] : 0; ring_[k + 1] = tx ? tx[i] : 0;
		}
		wpos_.store(w + n, std::memory_order_release);
	}
	// Once each, for the log: the size cap was reached; frames were dropped (since the last call).
	bool take_full_note() { return full_.load() && !full_noted_.exchange(true); }
	uint64_t take_dropped() { return dropped_.exchange(0); }
	void stop()
	{
		if (!f_) return;
		run_ = false;
		if (th_.joinable()) th_.join();
		drain();
		header(data_);
		std::fclose(f_); f_ = nullptr;
	}

private:
	static constexpr size_t kRing = 96000;           // frames: 10 s
	std::FILE *f_ = nullptr;
	std::vector<int16_t> ring_;
	std::atomic<uint64_t> wpos_{0}, rpos_{0}, dropped_{0};
	std::atomic<bool> run_{false}, full_{false}, full_noted_{false};
	std::thread th_;
	uint64_t max_bytes_ = 0, data_ = 0;
	std::mutex snap_m_;
	std::string snap_;
	std::map<long, uint64_t> prev_ticks_;
	uint64_t prev_all_ = 0, prev_iow_ = 0;
	double prev_t_ = 0;
	// load average, iowait and the three busiest processes since the last call (once a second)
	void take_snapshot()
	{
		const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
		char line[512];
		std::string out;
		if (std::FILE *f = std::fopen("/proc/loadavg", "r")) {
			double a = 0, b = 0, c = 0;
			if (std::fscanf(f, "%lf %lf %lf", &a, &b, &c) == 3) { std::snprintf(line, sizeof line, "load %.2f %.2f %.2f", a, b, c); out = line; }
			std::fclose(f);
		}
		if (std::FILE *f = std::fopen("/proc/stat", "r")) {
			unsigned long long v[8] = {};
			if (std::fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) == 8) {
				uint64_t all = 0; for (auto x : v) all += x;
				if (prev_all_ && all > prev_all_) { std::snprintf(line, sizeof line, ", iowait %.0f %%", 100.0 * double(v[4] - prev_iow_) / double(all - prev_all_)); out += line; }
				prev_all_ = all; prev_iow_ = v[4];
			}
			std::fclose(f);
		}
		const double hz = double(sysconf(_SC_CLK_TCK)), dt = prev_t_ > 0 ? now - prev_t_ : 0;
		std::map<long, uint64_t> cur;
		std::vector<std::pair<double, std::string>> top;
		if (DIR *d = opendir("/proc")) {
			while (dirent *e = readdir(d)) {
				char *end = nullptr;
				const long pid = std::strtol(e->d_name, &end, 10);
				if (pid <= 0 || *end) continue;
				std::snprintf(line, sizeof line, "/proc/%ld/stat", pid);
				std::FILE *f = std::fopen(line, "r");
				if (!f) continue;
				const size_t k = std::fread(line, 1, sizeof line - 1, f);
				std::fclose(f);
				line[k] = 0;
				char *o = std::strchr(line, '('), *c = std::strrchr(line, ')');
				if (!o || !c) continue;
				unsigned long long ut = 0, st = 0;
				// after ") ": state ppid pgrp session tty tpgid flags minflt cminflt majflt cmajflt utime stime
				if (std::sscanf(c + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &st) != 2) continue;
				cur[pid] = ut + st;
				const auto p = prev_ticks_.find(pid);
				if (dt > 0 && p != prev_ticks_.end() && ut + st > p->second)
					top.push_back({100.0 * double(ut + st - p->second) / hz / dt, std::string(o + 1, size_t(c - o - 1))});
			}
			closedir(d);
		}
		prev_ticks_.swap(cur); prev_t_ = now;
		std::sort(top.begin(), top.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
		if (!top.empty()) out += "; busiest over the last second:";
		for (size_t i = 0; i < top.size() && i < 3; i++) { std::snprintf(line, sizeof line, "%s %s %.0f %%", i ? "," : "", top[i].second.c_str(), top[i].first); out += line; }
		std::lock_guard<std::mutex> l(snap_m_);
		snap_ = out;
	}
	static void le32(unsigned char *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = uint8_t(v >> (8 * i)); }
	void header(uint64_t data)
	{
		unsigned char h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0,
		                       0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0};
		le32(h + 4, uint32_t(36 + data)); le32(h + 24, 9600); le32(h + 28, 9600 * 4); le32(h + 40, uint32_t(data));
		const long at = std::ftell(f_);
		std::fseek(f_, 0, SEEK_SET);
		std::fwrite(h, 1, sizeof h, f_);
		if (at > 44) std::fseek(f_, at, SEEK_SET);
		std::fflush(f_);
	}
	void drain()
	{
		const uint64_t w = wpos_.load(std::memory_order_acquire);
		uint64_t r = rpos_.load(std::memory_order_relaxed);
		while (r < w) {
			const size_t k = size_t(r % kRing), n = size_t(std::min<uint64_t>(w - r, kRing - k));
			size_t bytes = n * 4;
			if (data_ + bytes > max_bytes_) bytes = size_t((max_bytes_ - data_) / 4 * 4);
			if (bytes) { std::fwrite(&ring_[k * 2], 1, bytes, f_); data_ += bytes; }
			r += n;
			if (data_ + 4 > max_bytes_) { full_ = true; r = w; break; }
		}
		rpos_.store(r, std::memory_order_release);
	}
	void writer()
	{
		int k = 0;
		while (run_) {
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
			if (!full_) drain();
			if (++k % 5 == 0) { if (!full_) header(data_); take_snapshot(); }
		}
	}
};

}  // namespace signum

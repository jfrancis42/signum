// cpu_clock.hpp: the CPU clock held at its maximum while a link is up (a thin adapter: Linux
// cpufreq in sysfs; a no-op elsewhere).
//
// Why: an on-demand governor (a Raspberry Pi's default) drops the clock between packets (a Pi 4
// to 600 MHz), and P3 decodes then come late at every level: on the air the link backed off to
// SL1 and timed out, while with scaling_min_freq = scaling_max_freq the same links were flawless.
// So while a link is up (calling, setting up, linked) every cpufreq policy's scaling_min_freq is
// set to its scaling_max_freq, and in standby and on every exit the value found at start is
// written back. Nothing else is touched (not the governor, not the maximum).
//
// Writing scaling_min_freq needs root, or the file group-writable for the user: the example
// unit dist/signum-cpufreq.service does that at boot. Without it: one log line, and the clock
// is left alone.
//
// restore() is async-signal-safe (open/write/close of strings prepared at open), so a signal
// handler may call it.
//
// SIGNUM_TEST_CPUFREQ_ROOT (tests only): the directory that holds policy*/ instead of
// /sys/devices/system/cpu/cpufreq.
//
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
#pragma once

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef __linux__
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <vector>
#endif

namespace signum {

class CpuClock {
public:
	enum Mode { OFF, AUTO, MAX };
	static bool parse(const std::string &s, Mode &m)
	{
		if (s == "auto") m = AUTO;
		else if (s == "max") m = MAX;
		else if (s == "off") m = OFF;
		else return false;
		return true;
	}
	static const char *name(Mode m) { return m == AUTO ? "auto" : m == MAX ? "max" : "off"; }

	// Finds the policies and checks that their minimum can be written. msg: one line for the log
	// (empty when there is nothing to say). False: the clock is left alone from here on.
	bool open(Mode m, std::string &msg)
	{
		mode_ = m; n_ = 0; msg.clear(); denied_ = false;
		if (m == OFF) return false;
#ifdef __linux__
		const char *env = std::getenv("SIGNUM_TEST_CPUFREQ_ROOT");
		const std::string root = env && *env ? env : "/sys/devices/system/cpu/cpufreq";
		std::vector<std::string> pols;
		if (DIR *d = ::opendir(root.c_str())) {
			while (dirent *e = ::readdir(d)) if (!std::strncmp(e->d_name, "policy", 6)) pols.push_back(e->d_name);
			::closedir(d);
		}
		std::sort(pols.begin(), pols.end());
		if (pols.empty()) { msg = "[cpu] no cpufreq policies in " + root + ": the CPU clock is left alone"; return false; }
		std::string denied;
		for (const auto &p : pols) {
			if (n_ == kMax) break;
			const std::string dir = root + "/" + p;
			Pol &q = pol_[n_];
			if (!read_line(dir + "/scaling_min_freq", q.orig, sizeof q.orig) || !read_line(dir + "/scaling_max_freq", q.max, sizeof q.max)) continue;
			const std::string path = dir + "/scaling_min_freq";
			if (path.size() >= sizeof q.path) continue;
			std::memcpy(q.path, path.c_str(), path.size() + 1);
			const int fd = ::open(q.path, O_WRONLY);
			if (fd < 0) { denied = path; continue; }
			::close(fd);
			n_++;
		}
		if (!denied.empty()) {
			n_ = 0; denied_ = true;
			msg = "[cpu] cannot hold the CPU clock up (" + denied + " is not writable): it may drop between packets and P3 decodes then come late. "
			      "To allow it, make scaling_min_freq group-writable for this user at boot (example: the signum-cpufreq.service unit in the examples directory), or --cpu-clock off";
			return false;
		}
		if (n_ == 0) { msg = "[cpu] no readable cpufreq policy in " + root + ": the CPU clock is left alone"; return false; }
		char b[160];
		std::snprintf(b, sizeof b, "[cpu] CPU clock held at its maximum %s (%d polic%s, minimum %s kHz otherwise)", m == MAX ? "while running" : "while a link is up",
		              n_, n_ == 1 ? "y" : "ies", pol_[0].orig);
		msg = b;
		if (m == MAX) want(true);
		return true;
#else
		msg = "[cpu] --cpu-clock: not supported on this system (Linux cpufreq only)";
		return false;
#endif
	}
	// Called from the audio loop with "a link is up (calling, setting up, linked)". Writes only on a
	// change. True when it changed something (for one log line).
	bool want(bool link)
	{
		const bool hi = mode_ == MAX || (mode_ == AUTO && link);
		if (n_ == 0 || hi == bool(high_)) return false;
		high_ = hi;
		for (int i = 0; i < n_; i++) write_str(pol_[i].path, hi ? pol_[i].max : pol_[i].orig);
		return true;
	}
	bool high() const { return high_ != 0; }
	// cpufreq policies exist but their minimum cannot be written: the clock may drop between packets
	// (the CPU health indicator lists it as a cause; no policies at all is not. --cpu-clock off is flagged by the caller
	// with its own text: nothing holds the clock then, so the health text must not say it is held)
	bool unpinnable() const { return denied_; }
	// Back to the values found at open (exit paths; async-signal-safe).
	void restore()
	{
		if (!high_) return;
		high_ = 0;
		for (int i = 0; i < n_; i++) write_str(pol_[i].path, pol_[i].orig);
	}

private:
	static constexpr int kMax = 64;
	struct Pol { char path[256]; char orig[24]; char max[24]; };
	Pol pol_[kMax];
	int n_ = 0;
	Mode mode_ = OFF;
	bool denied_ = false;
	volatile std::sig_atomic_t high_ = 0;

	static bool read_line(const std::string &path, char *out, size_t n)
	{
		FILE *f = std::fopen(path.c_str(), "r");
		if (!f) return false;
		const bool ok = std::fgets(out, int(n), f) != nullptr;
		std::fclose(f);
		if (!ok) return false;
		out[std::strcspn(out, " \r\n")] = 0;
		return out[0] >= '0' && out[0] <= '9';
	}
	static void write_str(const char *path, const char *v)
	{
#ifdef __linux__
		const int fd = ::open(path, O_WRONLY | O_TRUNC);   // (as the shell's "echo > file"; a regular file in tests)
		if (fd < 0) return;
		(void)!::write(fd, v, std::strlen(v));
		::close(fd);
#else
		(void)path; (void)v;
#endif
	}
};

}  // namespace signum

// key_guard.hpp: a key-down watchdog that does not depend on the keying process surviving
// (--ic7300-power W --measure; docs/md/radio-ic7300.md "If Signum dies while the radio is keyed").
//
// --measure keys the IC-7300 over CI-V (1C 00 01). The radio stays keyed until it hears TX off (1C 00 00): if
// the measuring process is killed (SIGKILL, the OOM killer) between the two, nothing in that process can unkey
// it. ForkGuard forks a small process before the first key-down that holds the radio's serial port (the same
// open file) and a socket to the parent. It sends TX off (three times) and lowers RTS and DTR when:
//   - the parent dies, however it dies: the socket reads end-of-file (the kernel closes the parent's end), or
//     the parent's pid changes (getppid), or PR_SET_PDEATHSIG's SIGUSR1 arrives;
//   - a key-down outlives its deadline (arm(s): the parent keys only after the guard has acknowledged the
//     deadline; disarm() after TX off was read back). The guard reports the trip; the parent stops.
//   - the guard itself is asked to stop (SIGTERM).
// The guard ignores SIGINT/SIGQUIT/SIGHUP (Ctrl-C reaches the whole process group: the parent handles it and
// unkeys; the guard stays on watch until the parent has finished). If the guard dies instead, the parent sees
// it at the next arm() (no acknowledgement) and keys no more; its own per-step limit still unkeys the key-down
// in progress. Not covered: the host losing power (both processes die; what the radio does when its USB host
// disappears while keyed by CI-V is not known; the radio's own time-out timer was not used, see the doc).
//
// The guard never reads the port (the parent's answers stay the parent's); it writes whole CI-V frames only.
//
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
#pragma once

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace radio {

// What the measurement needs from a watchdog (tests give a fake one).
struct KeyGuard {
	virtual ~KeyGuard() = default;
	// Ready to watch the port fd (CI-V address addr). False and why: the caller must not key.
	virtual bool start(int fd, uint8_t addr, std::string &why) = 0;
	// The next key-down may last at most s seconds; true once the guard has acknowledged it.
	virtual bool arm(double s, std::string &why) = 0;
	// The key-down is over (TX off read back). False: the guard unkeyed the radio (its deadline) or is gone.
	virtual bool disarm(std::string &why) = 0;
	// Finished: the guard exits (without writing anything when disarmed).
	virtual void stop() = 0;
};

namespace guard_detail {
inline double now_s() { timespec t; ::clock_gettime(CLOCK_MONOTONIC, &t); return double(t.tv_sec) + t.tv_nsec * 1e-9; }
inline volatile sig_atomic_t &gone() { static volatile sig_atomic_t g = 0; return g; }
inline void on_gone(int) { gone() = 1; }
// TX off three times, 30 ms apart, then RTS and DTR low (a radio keyed by USB SEND = RTS drops too).
inline void unkey(int fd, uint8_t addr)
{
	const uint8_t f[] = {0xFE, 0xFE, addr, 0xE0, 0x1C, 0x00, 0x00, 0xFD};
	for (int k = 0; k < 3; k++) {
		if (::write(fd, f, sizeof f) < 0) {}
		timespec ts{0, 30 * 1000 * 1000};
		::nanosleep(&ts, nullptr);
	}
	int bits = TIOCM_RTS | TIOCM_DTR;
	::ioctl(fd, TIOCMBIC, &bits);
}
// The guard's loop (in the child). Never returns.
[[noreturn]] inline void run(int fd, int sock, uint8_t addr, pid_t parent)
{
	struct sigaction ign{}, term{};
	ign.sa_handler = SIG_IGN;
	for (int s : {SIGINT, SIGQUIT, SIGHUP, SIGPIPE}) ::sigaction(s, &ign, nullptr);
	term.sa_handler = on_gone;
	sigemptyset(&term.sa_mask);
	::sigaction(SIGTERM, &term, nullptr);
	::sigaction(SIGUSR1, &term, nullptr);
	::prctl(PR_SET_PDEATHSIG, SIGUSR1);
	if (::getppid() != parent) gone() = 1;              // the parent died before the line above
	bool armed = false;
	double deadline = 0;
	std::string in;
	for (;;) {
		pollfd p{sock, POLLIN, 0};
		const int r = ::poll(&p, 1, 20);
		bool eof = gone() || ::getppid() != parent;
		if (!eof && r > 0) {
			char b[64];
			const ssize_t n = ::read(sock, b, sizeof b);
			if (n <= 0 && !(n < 0 && (errno == EINTR || errno == EAGAIN))) eof = true;
			else if (n > 0) in.append(b, size_t(n));
		}
		size_t nl;
		while (!eof && (nl = in.find('\n')) != std::string::npos) {
			const std::string m = in.substr(0, nl);
			in.erase(0, nl + 1);
			if (m.rfind("ARM ", 0) == 0) {
				deadline = now_s() + std::atof(m.c_str() + 4);
				armed = true;
				if (::send(sock, "OK\n", 3, MSG_NOSIGNAL) < 0) eof = true;
			} else if (m == "DIS") {
				armed = false;
				if (::send(sock, "OK\n", 3, MSG_NOSIGNAL) < 0) eof = true;
			} else if (m == "BYE") {
				if (armed) unkey(fd, addr);
				::_exit(0);
			}
		}
		if (eof) { unkey(fd, addr); ::_exit(0); }   // the parent is gone: unkey whatever it was doing
		if (armed && now_s() > deadline) {
			unkey(fd, addr);
			armed = false;
			if (::send(sock, "TRIP\n", 5, MSG_NOSIGNAL) < 0) { ::_exit(0); }
		}
	}
}
}  // namespace guard_detail

// The real one: a forked process (see the top of this file).
class ForkGuard : public KeyGuard {
public:
	~ForkGuard() override { stop(); }
	bool start(int fd, uint8_t addr, std::string &why) override
	{
		stop();
		int sv[2];
		if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0) { why = std::string("socketpair: ") + std::strerror(errno); return false; }
		const pid_t parent = ::getpid();
		std::fflush(nullptr);
		const pid_t c = ::fork();
		if (c < 0) { why = std::string("fork: ") + std::strerror(errno); ::close(sv[0]); ::close(sv[1]); return false; }
		if (c == 0) { ::close(sv[0]); guard_detail::run(fd, sv[1], addr, parent); }
		::close(sv[1]);
		sock_ = sv[0]; pid_ = c;
		std::string e;
		if (!ask("DIS", e)) { why = "the key-down watchdog did not answer: " + e; stop(); return false; }
		return true;
	}
	bool arm(double s, std::string &why) override
	{
		char b[48];
		std::snprintf(b, sizeof b, "ARM %.3f", s);
		if (!ask(b, why)) { why = "the key-down watchdog: " + why; return false; }
		return true;
	}
	bool disarm(std::string &why) override
	{
		if (!ask("DIS", why)) { why = "the key-down watchdog: " + why; return false; }
		return true;
	}
	void stop() override
	{
		if (sock_ >= 0) { if (::send(sock_, "BYE\n", 4, MSG_NOSIGNAL) < 0) {} ::close(sock_); sock_ = -1; }
		if (pid_ > 0) {
			for (int k = 0; k < 100; k++) {             // it exits at once (BYE) or within 20 ms (EOF)
				if (::waitpid(pid_, nullptr, WNOHANG) == pid_) { pid_ = -1; break; }
				timespec ts{0, 10 * 1000 * 1000};
				::nanosleep(&ts, nullptr);
			}
			if (pid_ > 0) { ::kill(pid_, SIGTERM); ::waitpid(pid_, nullptr, 0); pid_ = -1; }
		}
	}
	pid_t pid() const { return pid_; }

private:
	int sock_ = -1;
	pid_t pid_ = -1;
	std::string in_;
	// One request, its OK within 1 s. A TRIP that came first: false ("unkeyed the radio").
	bool ask(const std::string &m, std::string &why)
	{
		if (sock_ < 0) { why = "not running"; return false; }
		const std::string s = m + "\n";
		if (::send(sock_, s.data(), s.size(), MSG_NOSIGNAL) != ssize_t(s.size())) { why = "gone (" + std::string(std::strerror(errno)) + ")"; return false; }
		const double end = guard_detail::now_s() + 1.0;
		bool trip = false;
		for (;;) {
			size_t nl;
			while ((nl = in_.find('\n')) != std::string::npos) {
				const std::string a = in_.substr(0, nl);
				in_.erase(0, nl + 1);
				if (a == "TRIP") trip = true;
				else if (a == "OK") {
					if (trip) { why = "its deadline passed and it unkeyed the radio"; return false; }
					return true;
				}
			}
			const double left = end - guard_detail::now_s();
			if (left <= 0) { why = "no answer"; return false; }
			pollfd p{sock_, POLLIN, 0};
			const int r = ::poll(&p, 1, int(left * 1000) + 1);
			if (r < 0 && errno == EINTR) continue;
			if (r <= 0) continue;
			char b[64];
			const ssize_t n = ::read(sock_, b, sizeof b);
			if (n == 0) { why = "gone"; return false; }
			if (n < 0) { if (errno == EINTR || errno == EAGAIN) continue; why = std::strerror(errno); return false; }
			in_.append(b, size_t(n));
		}
	}
};

}  // namespace radio

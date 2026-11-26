// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// The functions of rigctl.hpp (out of the header, so that they build in parallel).
#include "rigctl.hpp"

namespace radio {

int tcp_connect(const std::string &host, int port, int timeout_ms)
{
	addrinfo hints{}, *res = nullptr;
	hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
	if (::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) return -1;
	int fd = -1;
	for (addrinfo *a = res; a && fd < 0; a = a->ai_next) {
		fd = ::socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC, 0);
		if (fd < 0) continue;
		::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
		if (::connect(fd, a->ai_addr, a->ai_addrlen) != 0 && errno != EINPROGRESS) { ::close(fd); fd = -1; continue; }
		pollfd p{fd, POLLOUT, 0};
		int err = 0; socklen_t el = sizeof err;
		if (::poll(&p, 1, timeout_ms) != 1 || ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) != 0 || err) { ::close(fd); fd = -1; }
	}
	::freeaddrinfo(res);
	if (fd >= 0) { int one = 1; ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one); }
	return fd;
}

const std::vector<HamBand> & ham_bands()
{
	static const std::vector<HamBand> *b = new std::vector<HamBand>{
		{"160m", 1800, 2000}, {"80m", 3500, 4000}, {"60m", 5250, 5450}, {"40m", 7000, 7300}, {"30m", 10100, 10150},
		{"20m", 14000, 14350}, {"17m", 18068, 18168}, {"15m", 21000, 21450}, {"12m", 24890, 24990}, {"10m", 28000, 29700},
		{"6m", 50000, 54000}, {"4m", 70000, 70500}, {"2m", 144000, 148000}, {"1.25m", 219000, 225000}, {"70cm", 420000, 450000}};
	return *b;
}

const std::vector<RigSetting> & rig_settings()
{
	static const std::vector<RigSetting> &v = *new std::vector<RigSetting>{
		{"vfo", "VFO", "+v", "+V", "VFO", false},
		{"rit_hz", "RIT", "+j", "+J", "RIT", true},
		{"xit_hz", "XIT", "+z", "+Z", "XIT", true},
		{"ts_hz", "Tuning Step", "+n", "+N", "Tuning Step", true},
		{"split_freq_hz", "Split Freq", "+i", "+I", "TX Frequency", true},
		{"ctcss_tone", "CTCSS", "+c", "+C", "CTCSS Tone", true},                 // tenths of Hz, 0 off
		{"ctcss_sql", "CTCSS Squelch", "+\\get_ctcss_sql", "+\\set_ctcss_sql", "CTCSS Squelch", true},
		{"dcs_code", "DCS", "+d", "+D", "DCS Code", true},
		{"rptr_shift", "Repeater Duplex", "+r", "+R", "Repeater Shift", false},   // +, -, None
		{"rptr_offs_hz", "Repeater Offset", "+o", "+O", "Repeater Offset", true},
		{"powerstat", "Power Stat", "+\\get_powerstat", "+\\set_powerstat", "Power Status", true},
	};
	return v;
}

}  // namespace radio

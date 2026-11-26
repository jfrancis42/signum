// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// telnet.hpp: the terminal command interpreter on a raw TCP port (telnet), beside the pty and /shell.
//
// Each TCP client is one Core stream client (attach/input/take_output/detach), exactly as a pty
// client: terminal mode and JHOST4/JHOST1 host mode.
//
// Telnet handling (minimal), only while the client is in terminal mode:
//   - we never send negotiation first, so a plain socket sees exactly the TNC's bytes;
//   - once the client sends IAC, it is a telnet client: we offer WILL ECHO and WILL SGA. The
//     interpreter itself echoes nothing (it answers CR with its own new line), so this adapter
//     echoes for it: printable characters as typed, Backspace/DEL as BS SP BS, CR and the rest
//     not at all; no echo at the password prompt or in host mode. A plain socket (no IAC) gets
//     no echo, as on a serial line. We accept DO ECHO/DO SGA, refuse every other DO
//     (WONT), refuse every WILL except SGA (DONT), skip subnegotiation, ignore other commands;
//   - IAC IAC in is one 0xFF byte; to a telnet client 0xFF out is sent as IAC IAC; CR NUL and CR LF in are CR (the
//     LF of a telnet client's Enter would otherwise go out as an extra line in converse).
// Host mode is binary-clean: from the moment the interpreter enters JHOST (checked after every
// CR, so frames in the same packet as "JHOST4\r" are already raw), no IAC processing in either
// direction until it is back in terminal mode. A telnet client in host mode must therefore send
// and expect raw bytes (no IAC doubling), as a raw socket client does.
//
// Plain POSIX sockets, non-blocking, pumped from the core's loop thread (never blocks).
#pragma once
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#include "tnc_core.hpp"

namespace signum {

// Default TCP port: 3000, the port hardware TNCs of this kind commonly use for telnet access.
// This is the only place the default is defined.
constexpr int kTelnetDefaultPort = 3000;

class TelnetServer {
public:
	int port = kTelnetDefaultPort;      // 0 = off
	bool port_given = false;            // an explicit --telnet: a bind failure stops the program
	std::string bind_addr = "127.0.0.1";
	std::string password;               // --telnet-auth: prompt before attaching; empty = open

	// false (with err) only when the port was given explicitly and cannot be opened
	bool start(Core &core, std::string &err)
	{
		core_ = &core;
		if (port <= 0) return true;
		lfd_ = ::socket(AF_INET, SOCK_STREAM, 0);
		int one = 1;
		::setsockopt(lfd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
		sockaddr_in a{};
		a.sin_family = AF_INET;
		a.sin_port = htons(uint16_t(port));
		if (::inet_pton(AF_INET, bind_addr.c_str(), &a.sin_addr) != 1) { err = "--telnet-bind: not an IPv4 address: " + bind_addr; return fail(err); }
		if (::bind(lfd_, reinterpret_cast<sockaddr *>(&a), sizeof a) < 0 || ::listen(lfd_, 8) < 0) {
			err = "telnet: cannot listen on " + bind_addr + ":" + std::to_string(port) + ": " + std::strerror(errno);
			return fail(err);
		}
		::fcntl(lfd_, F_SETFL, O_NONBLOCK);
		std::fprintf(stderr, "[telnet] command interpreter on tcp %s:%d%s\n", bind_addr.c_str(), port,
		             password.empty() ? "" : " (password)");
		return true;
	}

	void pump()
	{
		if (lfd_ < 0) return;
		for (;;) {
			const int fd = ::accept(lfd_, nullptr, nullptr);
			if (fd < 0) break;
			::fcntl(fd, F_SETFL, O_NONBLOCK);
			int one = 1;
			::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
			Client &c = cl_[fd];
			if (password.empty()) c.id = core_->attach("telnet-" + std::to_string(fd));
			else c.out = "Password: ";
		}
		for (auto it = cl_.begin(); it != cl_.end();) {
			if (!service(it->first, it->second)) {
				if (it->second.id > 0) core_->detach(it->second.id);
				::close(it->first);
				it = cl_.erase(it);
			} else ++it;
		}
	}

	void stop()
	{
		for (auto &kv : cl_) { if (kv.second.id > 0 && core_) core_->detach(kv.second.id); ::close(kv.first); }
		cl_.clear();
		if (lfd_ >= 0) ::close(lfd_);
		lfd_ = -1;
	}

private:
	enum : uint8_t { SE = 240, SB = 250, WILL = 251, WONT = 252, DO = 253, DONT = 254, IAC = 255, O_ECHO = 1, O_SGA = 3 };
	struct Client {
		int id = 0;               // core stream client; 0 until the password is given
		bool telnet = false;      // has sent IAC
		int st = 0;               // IAC parser: 0 data, 1 IAC, 2 option byte, 3 SB, 4 SB+IAC
		uint8_t verb = 0;
		bool cr = false;          // last data byte was CR (for CR NUL)
		int tries = 0;
		std::string line;         // the password being typed
		std::string out;          // bytes waiting for the socket
	};

	bool fail(const std::string &err)
	{
		if (lfd_ >= 0) ::close(lfd_);
		lfd_ = -1;
		if (port_given) return false;
		std::fprintf(stderr, "[telnet] %s: telnet off (default port; give --telnet PORT or --telnet 0)\n", err.c_str());
		return true;
	}

	bool host(const Client &c) const { return c.id > 0 && core_->host_mode(c.id); }

	// one byte of terminal-mode input through the IAC parser; appends data bytes to d
	void iac_byte(Client &c, uint8_t b, std::string &d)
	{
		switch (c.st) {
		case 0:
			if (b == IAC) { c.st = 1; return; }
			if ((b == 0 || b == '\n') && c.cr) { c.cr = false; return; }   // CR NUL and CR LF (a telnet client's Enter) = CR
			c.cr = b == '\r';
			d += char(b);
			return;
		case 1:
			if (b == IAC) { c.st = 0; c.cr = false; d += char(0xFF); return; }
			if (!c.telnet) { c.telnet = true; c.out += std::string{char(IAC), char(WILL), char(O_ECHO), char(IAC), char(WILL), char(O_SGA)}; }
			if (b >= WILL) { c.verb = b; c.st = 2; return; }
			c.st = b == SB ? 3 : 0;
			return;
		case 2: {
			c.st = 0;
			std::string r;
			if (c.verb == DO && b != O_ECHO && b != O_SGA) r = {char(IAC), char(WONT), char(b)};
			if (c.verb == WILL && b != O_SGA) r = {char(IAC), char(DONT), char(b)};
			c.out += r;
			return;
		}
		case 3: if (b == IAC) c.st = 4; return;
		case 4: c.st = b == SE ? 0 : 3; return;
		}
	}

	void deliver(Client &c, const std::string &d)
	{
		if (c.id > 0) {
			if (c.telnet && !host(c))
				for (char ch : d) {
					if (ch >= 32 && ch < 127) c.out += ch;
					else if (ch == 8 || ch == 127) c.out += "\b \b";
				}
			core_->input(c.id, reinterpret_cast<const uint8_t *>(d.data()), d.size());
			return;
		}
		for (char ch : d) {                               // the password prompt
			if (ch == '\r' || ch == '\n') {
				if (c.line.empty() && ch == '\n') continue;
				if (c.line == password) { c.out += "\r\n"; c.id = core_->attach("telnet"); }
				else if (++c.tries >= 3) { c.out += "\r\n*** wrong password\r\n"; c.tries = 99; return; }
				else c.out += "\r\n*** wrong password\r\nPassword: ";
				c.line.clear();
				if (c.id > 0) return;                     // anything after the line is dropped
			} else if (ch != 0) c.line += ch;
		}
	}

	// false: drop the client
	bool service(int fd, Client &c)
	{
		uint8_t buf[4096];
		for (;;) {
			const ssize_t r = ::recv(fd, buf, sizeof buf, 0);
			if (r == 0) return false;
			if (r < 0) { if (errno == EAGAIN || errno == EWOULDBLOCK) break; if (errno == EINTR) continue; return false; }
			std::string d;
			for (ssize_t i = 0; i < r; i++) {
				if (host(c)) { d.append(reinterpret_cast<char *>(buf + i), size_t(r - i)); break; }
				iac_byte(c, buf[i], d);
				if (buf[i] == '\r' && c.st == 0) { deliver(c, d); d.clear(); }   // host mode may start here
			}
			if (!d.empty()) deliver(c, d);
		}
		if (c.id > 0) {
			std::string o = core_->take_output(c.id);
			if (c.telnet && !host(c)) {
				for (char ch : o) { c.out += ch; if (uint8_t(ch) == IAC) c.out += ch; }
			} else c.out += o;
		}
		while (!c.out.empty()) {
			const ssize_t w = ::send(fd, c.out.data(), c.out.size(), MSG_NOSIGNAL);
			if (w < 0) { if (errno == EAGAIN || errno == EWOULDBLOCK) break; if (errno == EINTR) continue; return false; }
			c.out.erase(0, size_t(w));
		}
		if (c.out.size() > (1u << 20)) return false;     // a client that never reads
		return c.tries < 99 || !c.out.empty();
	}

	Core *core_ = nullptr;
	int lfd_ = -1;
	std::map<int, Client> cl_;
};

}  // namespace signum

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// ws_server.hpp: signum's WebSocket server, with three
// additions for the command shell on a WebSocket (src/web/frontend.cpp):
//   - Inbound carries the request path of the upgrade (on the open event),
//     so /shell clients can be told from JSON clients, and whether a
//     message came as a binary frame;
//   - post_bytes(): a binary frame to one client (raw interpreter bytes);
//   - close_client(): a close frame, then the connection is dropped.
// Clients whose path starts with /shell are subscribed to no stream.
//
// Original header:
// ws_server.hpp: the station's WebSocket interface (TODO M): a small
// RFC 6455 server plus static files over HTTP (the GUI page), on its own
// thread. The audio loop never blocks on it: what it posts and takes passes
// through queues guarded by try_lock, as src/util/async_out.hpp does.
//
// The host interface (virtual serial port, terminal and host mode) is not
// touched by this; the WebSocket is an additional way in (user, 2026-10-20).
//
// Wire format: JSON text frames, protocol in src/web/README.md.
// Credentials (user, 2026-10-20; v1: from the command line): open by
// default. web_auth "user:pass" puts HTTP Basic on the pages; ws_auth
// "user:pass" on the WebSocket (a Basic header, ?auth=base64(user:pass), or
// an "auth" message first, checked by the station's loop). Separate: the
// WebSocket stays open unless ws_auth is given.
// POSIX sockets only (lwIP on the ESP32 has the same calls).
#pragma once
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <set>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace wss {

// ---- SHA-1 and base64 (the handshake only) ---------------------------------
inline std::string sha1(const std::string &msg)
{
	uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
	std::string m = msg;
	const uint64_t bits = uint64_t(msg.size()) * 8;
	m += char(0x80);
	while (m.size() % 64 != 56) m += char(0);
	for (int i = 7; i >= 0; i--) m += char((bits >> (8 * i)) & 0xFF);
	auto rol = [](uint32_t v, int s) { return (v << s) | (v >> (32 - s)); };
	for (size_t off = 0; off < m.size(); off += 64) {
		uint32_t w[80];
		for (int i = 0; i < 16; i++)
			w[i] = uint32_t((unsigned char)m[off + 4 * i]) << 24 | uint32_t((unsigned char)m[off + 4 * i + 1]) << 16 |
			       uint32_t((unsigned char)m[off + 4 * i + 2]) << 8 | uint32_t((unsigned char)m[off + 4 * i + 3]);
		for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
		uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
		for (int i = 0; i < 80; i++) {
			uint32_t f, k;
			if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
			else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
			else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
			else { f = b ^ c ^ d; k = 0xCA62C1D6; }
			const uint32_t t = rol(a, 5) + f + e + k + w[i];
			e = d; d = c; c = rol(b, 30); b = a; a = t;
		}
		h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
	}
	std::string out;
	for (uint32_t v : h) for (int i = 3; i >= 0; i--) out += char((v >> (8 * i)) & 0xFF);
	return out;
}
inline std::string base64(const std::string &in)
{
	static const char *T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string o;
	size_t i = 0;
	for (; i + 2 < in.size(); i += 3) {
		const uint32_t v = uint32_t((unsigned char)in[i]) << 16 | uint32_t((unsigned char)in[i + 1]) << 8 | (unsigned char)in[i + 2];
		o += T[v >> 18]; o += T[(v >> 12) & 63]; o += T[(v >> 6) & 63]; o += T[v & 63];
	}
	if (i + 1 == in.size()) { const uint32_t v = uint32_t((unsigned char)in[i]) << 16; o += T[v >> 18]; o += T[(v >> 12) & 63]; o += "=="; }
	else if (i + 2 == in.size()) { const uint32_t v = uint32_t((unsigned char)in[i]) << 16 | uint32_t((unsigned char)in[i + 1]) << 8; o += T[v >> 18]; o += T[(v >> 12) & 63]; o += T[(v >> 6) & 63]; o += '='; }
	return o;
}

// ---- the server -------------------------------------------------------------
struct Inbound {
	int client;          // id
	std::string text;    // a JSON message, or "" with event set
	int event = 0;       // 1: client opened, 2: client closed
	std::string path;    // event 1: the upgrade request's path (with its query)
	bool binary = false; // a binary frame (else text)
};

class Server {
public:
	~Server() { stop(); }
	// addr "127.0.0.1" (default) or e.g. "0.0.0.0"; credentials "" = open (see above).
	// a loopback address. docroot: the directory served over HTTP ("" none).
	bool start(const std::string &addr, int port, const std::string &web_auth, const std::string &ws_auth, const std::string &docroot, std::string &err)
	{
		web_auth_ = web_auth; ws_auth_ = ws_auth; docroot_ = docroot;
		lfd_ = ::socket(AF_INET, SOCK_STREAM, 0);
		if (lfd_ < 0) { err = "socket"; return false; }
		int one = 1;
		setsockopt(lfd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
		sockaddr_in sa{};
		sa.sin_family = AF_INET; sa.sin_port = htons(uint16_t(port));
		if (inet_pton(AF_INET, addr.c_str(), &sa.sin_addr) != 1) { err = "bad address " + addr; ::close(lfd_); lfd_ = -1; return false; }
		if (::bind(lfd_, reinterpret_cast<sockaddr *>(&sa), sizeof sa) < 0 || ::listen(lfd_, 8) < 0) {
			err = std::string("bind ") + addr + ":" + std::to_string(port) + ": " + std::strerror(errno);
			::close(lfd_); lfd_ = -1; return false;
		}
		fcntl(lfd_, F_SETFL, fcntl(lfd_, F_GETFL) | O_NONBLOCK);
		run_ = true;
		th_ = std::thread([this] { loop(); });
		return true;
	}
	void stop()
	{
		if (!run_) return;
		run_ = false;
		if (th_.joinable()) th_.join();
		for (auto &c : cl_) ::close(c.fd);
		cl_.clear();
		if (lfd_ >= 0) ::close(lfd_);
		lfd_ = -1;
	}
	bool running() const { return run_; }
	// Set before start(): HTTP requests the station answers (method, path, content-type, body ->
	// status, content-type, body; false: not ours, served from the docroot as before).
	std::function<bool(const std::string &, const std::string &, const std::string &, const std::string &, std::string &, std::string &, std::string &)> http_handler;
	int clients() const { return nclients_.load(); }

	// From the station's loop: a message to every client subscribed to
	// `stream` (client = -1), or to one client. Never blocks.
	void post(const std::string &stream, const std::string &json, int client = -1) { pend_.push_back({client, stream, json}); }
	void commit()
	{
		if (pend_.empty()) return;
		std::unique_lock<std::mutex> lk(omx_, std::try_to_lock);
		if (!lk.owns_lock()) { if (pend_.size() > 20000) pend_.erase(pend_.begin(), pend_.begin() + 10000); return; }
		for (auto &p : pend_) out_.push_back(std::move(p));
		pend_.clear();
	}
	// From the station's loop: messages received (and opens / closes).
	void take(std::vector<Inbound> &v)
	{
		std::unique_lock<std::mutex> lk(imx_, std::try_to_lock);
		if (!lk.owns_lock()) return;
		while (!in_.empty()) { v.push_back(std::move(in_.front())); in_.pop_front(); }
	}
	// Subscriptions: which streams a client gets (set from the station's loop
	// through a message; applied on the server thread).
	void subscribe(int client, const std::vector<std::string> &streams) { post("_subscribe", join(streams), client); }
	// From the station's loop: bytes as one binary frame to one client.
	void post_bytes(int client, const std::string &bytes) { pend_.push_back({client, "_bin", bytes}); }
	// From the station's loop: a close frame (code 1008 when policy, else 1000), then drop it.
	void close_client(int client, bool policy = false) { pend_.push_back({client, policy ? "_close8" : "_close", ""}); }

private:
	struct Out { int client; std::string stream, json; };
	struct Client {
		int id, fd;
		bool ws = false, closing = false, dead = false;   // dead: the peer is gone (nothing more can be sent)
		std::string in, out;
		std::set<std::string> subs{"panel", "rx", "link", "event", "config", "rig", "monitor", "mail", "system", "crypto"};
		std::string frag;                       // a fragmented message being reassembled
		bool frag_bin = false;
		std::string path;
	};
	int lfd_ = -1, next_id_ = 1;
	std::atomic<bool> run_{false};
	std::atomic<int> nclients_{0};
	std::thread th_;
	std::string web_auth_, ws_auth_, docroot_;
	std::vector<Client> cl_;
	std::vector<Out> pend_;                     // station side, not yet handed over
	std::mutex omx_, imx_;
	std::vector<Out> out_;
	std::deque<Inbound> in_;

	static std::string join(const std::vector<std::string> &v) { std::string s; for (auto &x : v) s += (s.empty() ? "" : ",") + x; return s; }
	void push_in(Inbound m) { std::lock_guard<std::mutex> lk(imx_); if (in_.size() < 2000) in_.push_back(std::move(m)); }

	void loop()
	{
		while (run_) {
			// hand over what the station posted
			std::vector<Out> o;
			{ std::lock_guard<std::mutex> lk(omx_); o.swap(out_); }
			for (auto &m : o) {
				for (auto &c : cl_) {
					if (!c.ws || (m.client >= 0 && m.client != c.id)) continue;
					if (m.stream == "_subscribe") {
						c.subs.clear();
						size_t a = 0;
						while (a <= m.json.size()) { const size_t b = m.json.find(',', a); const std::string s = m.json.substr(a, b == std::string::npos ? std::string::npos : b - a); if (!s.empty()) c.subs.insert(s); if (b == std::string::npos) break; a = b + 1; }
						continue;
					}
					if (m.stream == "_bin") { frame(c, m.json, 0x2); continue; }
					if (m.stream == "_close" || m.stream == "_close8") {
						frame(c, std::string(m.stream == "_close8" ? "\x03\xf0" : "\x03\xe8", 2), 0x8);
						c.closing = true;
						continue;
					}
					if (m.client < 0 && !c.subs.count(m.stream)) continue;
					frame(c, m.json, 0x1);
				}
			}
			std::vector<pollfd> pf;
			pf.push_back({lfd_, POLLIN, 0});
			for (auto &c : cl_) pf.push_back({c.fd, short(POLLIN | (c.out.empty() ? 0 : POLLOUT)), 0});
			::poll(pf.data(), pf.size(), 15);
			if (pf[0].revents & POLLIN) accept_one();
			for (size_t i = 0; i < cl_.size(); i++) {
				Client &c = cl_[i];
				const short re = pf.size() > i + 1 ? pf[i + 1].revents : 0;
				if (re & (POLLIN | POLLHUP | POLLERR)) {
					char b[4096];
					const ssize_t k = ::recv(c.fd, b, sizeof b, 0);
					if (k <= 0) c.closing = c.dead = true;
					else { c.in.append(b, size_t(k)); if (c.in.size() > (c.ws ? (2u << 20) : (5u << 20))) c.closing = true; else handle(c); }   // HTTP: bodies up to 4 MB (a forms zip)
				}
				if (!c.out.empty()) {
					const ssize_t k = ::send(c.fd, c.out.data(), c.out.size(), MSG_NOSIGNAL);
					if (k > 0) c.out.erase(0, size_t(k));
					else if (k < 0 && errno != EAGAIN && errno != EWOULDBLOCK) c.closing = c.dead = true;
					if (c.out.size() > (4u << 20)) c.closing = c.dead = true;   // a client that does not read
				}
			}
			for (size_t i = 0; i < cl_.size();) {
				if (cl_[i].closing && (cl_[i].out.empty() || cl_[i].dead)) {   // the whole reply sent first (a page cut off over the LAN, 2026-10-23)
					if (cl_[i].ws) push_in({cl_[i].id, "", 2, "", false});
					::close(cl_[i].fd); cl_.erase(cl_.begin() + long(i));
				} else i++;
			}
			nclients_ = int(cl_.size());
		}
	}
	void accept_one()
	{
		sockaddr_in sa{}; socklen_t sl = sizeof sa;
		const int fd = ::accept(lfd_, reinterpret_cast<sockaddr *>(&sa), &sl);
		if (fd < 0) return;
		if (cl_.size() >= 16) { ::close(fd); return; }
		fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
		int one = 1;
		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
		Client c; c.id = next_id_++; c.fd = fd;
		cl_.push_back(std::move(c));
	}
	static std::string header(const std::string &req, const std::string &name)
	{
		size_t p = 0;
		while ((p = req.find("\r\n", p)) != std::string::npos) {
			p += 2;
			const size_t e = req.find("\r\n", p), col = req.find(':', p);
			if (col == std::string::npos || col > e) continue;
			std::string k = req.substr(p, col - p);
			for (auto &ch : k) ch = char(std::tolower((unsigned char)ch));
			if (k == name) { std::string v = req.substr(col + 1, e - col - 1); while (!v.empty() && v[0] == ' ') v.erase(0, 1); return v; }
		}
		return "";
	}
	static std::string query(const std::string &path, const std::string &key)
	{
		const size_t q = path.find('?');
		if (q == std::string::npos) return "";
		size_t a = q + 1;
		while (a < path.size()) {
			const size_t b = path.find('&', a), eq = path.find('=', a);
			const std::string k = path.substr(a, (eq == std::string::npos || (b != std::string::npos && eq > b) ? b : eq) - a);
			if (k == key && eq != std::string::npos && (b == std::string::npos || eq < b)) return path.substr(eq + 1, b == std::string::npos ? std::string::npos : b - eq - 1);
			if (b == std::string::npos) break;
			a = b + 1;
		}
		return "";
	}
	void http_reply(Client &c, const char *status, const std::string &type, const std::string &body)
	{
		c.out += std::string("HTTP/1.1 ") + status + "\r\nContent-Type: " + type + "\r\nContent-Length: " + std::to_string(body.size()) +
		         "\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n" + body;
		c.closing = true;
	}
	void handle(Client &c)
	{
		if (!c.ws) {
			const size_t e = c.in.find("\r\n\r\n");
			if (e == std::string::npos) return;
			const std::string req = c.in.substr(0, e + 2);
			c.in.erase(0, e + 4);
			const size_t s1 = req.find(' '), s2 = req.find(' ', s1 + 1);
			const std::string path = s1 == std::string::npos || s2 == std::string::npos ? "/" : req.substr(s1 + 1, s2 - s1 - 1);
			std::string up = header(req, "upgrade");
			for (auto &ch : up) ch = char(std::tolower((unsigned char)ch));
			const std::string basic = header(req, "authorization");
			auto basic_is = [&](const std::string &cred) { return !cred.empty() && (basic == "Basic " + base64(cred) || query(path, "auth") == base64(cred)); };
			if (up != "websocket" && !web_auth_.empty() && !basic_is(web_auth_)) {
				c.out += "HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: Basic realm=\"TNC station\"\r\nContent-Length: 13\r\nConnection: close\r\n\r\nlogin needed\n";
				c.closing = true;
				return;
			}
			// Requests the station answers itself (Winlink forms, mail_forms.hpp): method, path,
			// body (POST: Content-Length, up to 4 MB, waited for). On the server's thread.
			if (up != "websocket" && http_handler) {
				const std::string method = req.substr(0, s1 == std::string::npos ? 0 : s1);
				const long clen = std::atol(header(req, "content-length").c_str());
				if (clen < 0 || clen > (4 << 20)) { http_reply(c, "413 Payload Too Large", "text/plain", "too large\n"); return; }
				if (c.in.size() < size_t(clen)) { c.in.insert(0, req + "\r\n"); return; }   // the body is not all in: look again later
				const std::string body = c.in.substr(0, size_t(clen));
				c.in.erase(0, size_t(clen));
				std::string status = "200 OK", type = "text/html; charset=utf-8", out;
				if (http_handler(method, path, header(req, "content-type"), body, status, type, out)) { http_reply(c, status.c_str(), type, out); return; }
			}
			if (up == "websocket") {
				const std::string key = header(req, "sec-websocket-key");
				if (key.empty()) { http_reply(c, "400 Bad Request", "text/plain", "no key\n"); return; }
				c.out += "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " +
				         base64(sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11")) + "\r\n\r\n";
				c.ws = true;
				c.path = path;
				if (path.compare(0, 6, "/shell") == 0) c.subs.clear();
				{
					Inbound o{c.id, ws_auth_.empty() || basic_is(ws_auth_) ? "authed" : "", 1, "", false};
					o.path = path;
					push_in(std::move(o));
				}   // open; "authed": no credentials needed or given in the request
				if (!c.in.empty()) frames(c);
				return;
			}
			serve_file(c, path);
			return;
		}
		frames(c);
	}
	void serve_file(Client &c, std::string path)
	{
		const size_t q = path.find('?');
		if (q != std::string::npos) path.erase(q);
		if (path == "/" || path.empty()) path = "/index.html";
		if (docroot_.empty() || path.find("..") != std::string::npos || path[0] != '/') { http_reply(c, "404 Not Found", "text/plain", "not found\n"); return; }
		FILE *f = std::fopen((docroot_ + path).c_str(), "rb");
		if (!f) { http_reply(c, "404 Not Found", "text/plain", "not found\n"); return; }
		std::string body;
		char b[8192]; size_t k;
		while ((k = std::fread(b, 1, sizeof b, f)) > 0) body.append(b, k);
		std::fclose(f);
		auto ends = [&](const char *x) { const size_t n = std::strlen(x); return path.size() >= n && path.compare(path.size() - n, n, x) == 0; };
		const std::string type = ends(".html") ? "text/html; charset=utf-8" : ends(".js") ? "text/javascript" : ends(".css") ? "text/css" :
		                         ends(".svg") ? "image/svg+xml" : ends(".png") ? "image/png" : ends(".json") ? "application/json" : "application/octet-stream";
		http_reply(c, "200 OK", type, body);
	}
	void frames(Client &c)
	{
		for (;;) {
			if (c.in.size() < 2) return;
			const unsigned char *p = reinterpret_cast<const unsigned char *>(c.in.data());
			const bool fin = p[0] & 0x80, masked = p[1] & 0x80;
			const int op = p[0] & 0x0F;
			uint64_t len = p[1] & 0x7F;
			size_t h = 2;
			if (len == 126) { if (c.in.size() < 4) return; len = uint64_t(p[2]) << 8 | p[3]; h = 4; }
			else if (len == 127) { if (c.in.size() < 10) return; len = 0; for (int i = 0; i < 8; i++) len = len << 8 | p[2 + i]; h = 10; }
			if (len > (1u << 20)) { c.closing = true; return; }
			const size_t mh = masked ? 4 : 0;
			if (c.in.size() < h + mh + len) return;
			std::string pay = c.in.substr(h + mh, size_t(len));
			if (masked) for (size_t i = 0; i < pay.size(); i++) pay[i] = char(pay[i] ^ p[h + (i & 3)]);
			c.in.erase(0, h + mh + size_t(len));
			if (op == 0x8) { frame(c, pay.substr(0, 2), 0x8); c.closing = true; return; }   // close: echo the code
			if (op == 0x9) { frame(c, pay, 0xA); continue; }                                  // ping -> pong
			if (op == 0xA) continue;
			if (op == 0x1 || op == 0x2) { c.frag = pay; c.frag_bin = op == 0x2; }
			else if (op == 0x0) c.frag += pay;
			else continue;
			if (c.frag.size() > (1u << 20)) { c.closing = true; return; }
			if (fin) { Inbound o{c.id, c.frag, 0, "", c.frag_bin}; push_in(std::move(o)); c.frag.clear(); }
		}
	}
	static void frame(Client &c, const std::string &pay, int op)
	{
		std::string f;
		f += char(0x80 | op);
		if (pay.size() < 126) f += char(pay.size());
		else if (pay.size() < 65536) { f += char(126); f += char(pay.size() >> 8); f += char(pay.size() & 0xFF); }
		else { f += char(127); for (int i = 7; i >= 0; i--) f += char((uint64_t(pay.size()) >> (8 * i)) & 0xFF); }
		c.out += f;
		c.out += pay;
	}
};

}  // namespace wss

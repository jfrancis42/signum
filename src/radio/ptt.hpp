// ptt.hpp: keying a radio, for every radio our station drives (the HF radio
// here).
//
//   rts:TTY            RTS on a serial port (Icom USB: menu USB SEND = RTS;
//                      any interface that keys on RTS)
//   dtr:TTY            DTR (Icom USB SEND = DTR; interfaces wired to DTR)
//   civ:TTY:ADDR[:BAUD]  Icom CI-V "TX on/off" (1C 00 01/00); ADDR hex, required
//                      (IC-7300 94, IC-9700 A2, IC-7610 98), BAUD default
//                      19200 (the radio's CI-V USB baud rate must match).
//                      As with rig: while unkeyed (after 0.3 s) the TX state
//                      is read (1C 00) every 0.5 s, and a radio still keyed
//                      gets "TX off" again
//   rig[:HOST[:PORT]]  through Hamlib's rigctld ("T 1" / "T 0") on a connection
//                      of its own; without HOST:PORT the station's rig
//                      control's rigctld (src/radio/rigctl.hpp). Slower
//                      than RTS: CAT latency is unmeasured for the link timing.
//                      While unkeyed (after 0.3 s) the radio's PTT is read
//                      ("t") every 0.5 s, and a radio still keyed gets "T 0"
//                      again: a radio also keyed by an interface's VOX PTT
//                      ignores the unkey while that line is held, and then
//                      stays keyed (seen with a KX2 + a VOX-keying interface).
//                      The front panel's frequency readout comes from here too
//                      (freq_poll): in standby only, after 2 s unkeyed, every
//                      2 s the "t" poll is replaced by "f" (read the frequency;
//                      read only, the radio is never set): no more CAT traffic
//                      than before, and none during a call or a link, where a
//                      read in flight could hold up a "T 1"
//   vox                the radio keys on our audio (no line touched; not
//                      for this protocol, whose timing a VOX delay breaks)
//   none               never key (receive only, or a test)
// TTY may be a /dev/serial/by-id/ path (stable across boots and replugs).
//
// Safety (the bench rules): RTS and DTR are held low from open to close, and
// the port is opened, used and left at speed B0 where it can be, so opening it
// does not key a radio that keys on RTS/DTR (src/util/serial_safe.hpp); an
// unkey on an Icom also sends CI-V "TX off" when ADDR is known; a key-down
// watchdog drops the key after max_key_s of continuous keying and holds it
// down until the caller itself unkeys; nothing here blocks (the port is
// non-blocking). Only RTS/DTR and CI-V TX on/off are ever touched: never
// power or anything else.
//
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
#pragma once

#include "rigctl.hpp"                   // tcp_connect (the rig kind)
#include "../util/serial_safe.hpp"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <netinet/in.h>
#include <sys/socket.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace radio {

enum class PttKind { NONE, RTS, DTR, CIV, VOX, RIG };

struct PttSpec {
	PttKind kind = PttKind::NONE;
	std::string tty;
	uint8_t civ_addr = 0;          // 0 = unknown (no CI-V TX off on unkey)
	int civ_baud = 19200;
	std::string rig_host;          // rig: "" = the station's rigctld
	int rig_port = 0;
	// "rts:/dev/ttyUSB1", "civ:/dev/ttyUSB0:94:115200", "vox", "none".
	// False when it does not parse.
	bool parse(const std::string &s)
	{
		auto field = [&](size_t i) {
			size_t a = 0;
			for (size_t k = 0; k < i; k++) { a = s.find(':', a); if (a == std::string::npos) return std::string(); a++; }
			const size_t b = s.find(':', a);
			return s.substr(a, b == std::string::npos ? std::string::npos : b - a);
		};
		const std::string k = field(0);
		if (k == "none") { kind = PttKind::NONE; return true; }
		if (k == "vox") { kind = PttKind::VOX; return true; }
		if (k == "rig") {                                // rig[:HOST[:PORT]]
			kind = PttKind::RIG; tty.clear();
			rig_host = field(1); const std::string pt = field(2);
			rig_port = pt.empty() ? 0 : std::atoi(pt.c_str());
			return true;
		}
		kind = k == "rts" ? PttKind::RTS : k == "dtr" ? PttKind::DTR : k == "civ" ? PttKind::CIV : PttKind::NONE;
		if (kind == PttKind::NONE) return false;
		tty = field(1);
		if (tty.empty()) return false;
		const std::string a = field(2), b = field(3);
		if (!a.empty()) civ_addr = uint8_t(std::strtoul(a.c_str(), nullptr, 16));
		if (!b.empty()) civ_baud = std::atoi(b.c_str());
		return kind != PttKind::CIV || civ_addr != 0;
	}
	bool keys_line() const { return kind == PttKind::RTS || kind == PttKind::DTR || kind == PttKind::CIV; }
	std::string text() const
	{
		static const char *const n[6] = {"none", "rts", "dtr", "civ", "vox", "rig"};
		std::string t = n[int(kind)];
		if (kind == PttKind::RIG) return rig_host.empty() ? t : t + ":" + rig_host + (rig_port ? ":" + std::to_string(rig_port) : "");
		if (!tty.empty()) t += ":" + tty;
		if (civ_addr) { char b[8]; std::snprintf(b, sizeof b, ":%02X", civ_addr); t += b; }
		return t;
	}
};

class Ptt {
public:
	~Ptt() { close(); }
	bool open(const PttSpec &s, double max_key_s)
	{
		spec_ = s; max_key_ = max_key_s;
		if (s.kind == PttKind::RIG) {                    // rigctld: connect (it may still be starting: retry ~5 s)
			for (int i = 0; i < 25 && fd_ < 0; i++) { fd_ = tcp_connect(s.rig_host.empty() ? "127.0.0.1" : s.rig_host, s.rig_port ? s.rig_port : 4532, 200); if (fd_ < 0) ::usleep(200000); }
			if (fd_ < 0) { std::fprintf(stderr, "[ptt] no rigctld at %s\n", s.text().c_str()); return false; }
			raw_key(false);
			return true;
		}
		if (s.tty.empty()) return s.kind == PttKind::NONE || s.kind == PttKind::VOX;
		// RTS/DTR low from the open on, the port used and left at B0 (src/util/serial_safe.hpp): no
		// key-up at the open. RTS/DTR keying needs no rate (rigctld may share the port).
		if (!port_.open(s.tty, s.kind == PttKind::CIV ? s.civ_baud : 0)) { std::fprintf(stderr, "[ptt] %s\n", port_.note().c_str()); return false; }
		if (!port_.note().empty()) std::fprintf(stderr, "[ptt] %s\n", port_.note().c_str());
		if (s.kind == PttKind::CIV && s.civ_addr && port_.rate_unknown() && !civ_answers()) {
			// at B0 the chip has the rate it was last given: not the radio's, so set it (a short key-up)
			std::fprintf(stderr, "[ptt] no CI-V answer at the port's last rate: setting %d Bd (leaving B0 raises RTS/DTR for an instant)\n", s.civ_baud);
			port_.set_rate_now();
			if (!civ_answers()) std::fprintf(stderr, "[ptt] WARNING: no CI-V answer from %02X at %d Bd: is the radio on, its CI-V address and USB baud rate those?\n", s.civ_addr, s.civ_baud);
		}
		raw_key(false);
		return true;
	}
	void close()
	{
		if (spec_.kind == PttKind::RIG) { if (fd_ >= 0) { raw_key(false); ::close(fd_); fd_ = -1; } }
		else if (port_.fd() >= 0) { raw_key(false); port_.close(); }   // lines low, port at B0
		keyed_ = false;
	}
	// Unkeyed, lines low, the port at B0, still open (async-signal-safe: exit paths that end the
	// process at once, crash handlers).
	void rest_now() { force_off(); if (spec_.kind != PttKind::RIG) port_.rest(); }
	void use_sys(util::SerialSys *s) { port_.use(s); }   // a test's fake OS calls
	// Called every audio period with what the station wants and the time (s,
	// any monotonic clock). Keys on change; enforces the watchdog.
	void set(bool want, double now)
	{
		if (held_) want = false;
		if (!want) tripped_ = false;
		civ_retry(want && !tripped_);
		if (want && !tripped_ && !keyed_) { raw_key(true); keyed_ = true; t_key_ = now; }
		else if ((!want || tripped_) && keyed_) { raw_key(false); keyed_ = false; t_unkey_ = now; }
		if (!keyed_ && spec_.kind == PttKind::RIG && fd_ >= 0) rig_check(now);
		if (!keyed_ && spec_.kind == PttKind::CIV && spec_.civ_addr && port_.fd() >= 0) civ_check(now);
		if (keyed_ && max_key_ > 0 && now - t_key_ > max_key_) {
			raw_key(false); keyed_ = false; tripped_ = true; trips_++;
			std::fprintf(stderr, "[ptt] WATCHDOG: keyed %.0f s, key dropped (%s)\n", now - t_key_, spec_.text().c_str());
		}
	}
	// Unkey at once (exit paths, signal handlers: async-signal-safe).
	void force_off() { if (fd_ >= 0 || port_.fd() >= 0) raw_key(false); keyed_ = false; }
	bool keyed() const { return keyed_; }
	// Held: never keyed (the radio's settings are being changed: src/radio/ic7300.hpp); unkeys at once.
	void hold(bool h) { held_ = h; if (h && keyed_) { raw_key(false); keyed_ = false; } }
	bool held() const { return held_; }
	// rig: the radio is transmitting on its own (its tune cycle, src/radio/rig_tune.hpp) while we are not keying: the
	// stuck-key check must not unkey it ("T 0"). While on, the PTT is not polled; 0.3 s after it ends, as after an unkey.
	void external_tx(bool on, double now) { if (ext_tx_ && !on) t_unkey_ = now; ext_tx_ = on; }
	void set_max_key(double s) { max_key_ = s; }   // the key-down watchdog, changed while running
	long trips() const { return trips_; }
	long rig_unkeys() const { return rig_unkeys_; }   // times the radio was found keyed while we were not (rig: and civ:)
	long civ_rewrites() const { return civ_rewrites_; }   // civ: TX on/off commands written again (not taken whole by the port)
	// rig: the radio's frequency for the front panel. freq_poll(true) while the station is in standby (no call, no
	// link): only then is it read. freq_hz(): -1 no rig control, 0 not known (none read yet, or no answer for 10 s).
	void freq_poll(bool on) { freq_on_ = on; }
	double freq_hz(double now) const
	{
		if (spec_.kind != PttKind::RIG || fd_ < 0) return -1;
		// stale after 10 s without an answer in standby; during a call or a link (not read then) the last one stands
		return freq_hz_ > 0 && (!freq_on_ || now - freq_t_ < 10.0) ? freq_hz_ : 0.0;
	}
	const PttSpec &spec() const { return spec_; }

private:
	PttSpec spec_;
	int fd_ = -1;                    // rig: the rigctld connection
	util::SafePort port_;            // rts/dtr/civ: the serial port
	bool keyed_ = false, tripped_ = false, held_ = false;
	double t_key_ = 0, max_key_ = 0;
	long trips_ = 0;
	// rig: the PTT check while unkeyed
	double t_unkey_ = -1e9, t_poll_ = -1e9;
	bool rig_stuck_ = false, ext_tx_ = false;
	long rig_unkeys_ = 0;
	char line_[32];
	int line_n_ = 0;
	bool freq_on_ = false;           // rig: the frequency may be read now (standby)
	double freq_hz_ = 0, freq_t_ = -1e9, freq_ask_ = -1e9;
	int freq_wait_ = 0;              // "f" asked, answers not yet read
	uint8_t civ_[16];                // civ: a CI-V frame being read
	int civ_n_ = 0;
	int civ_pend_ = -1;              // civ: a TX on (1) / off (0) the port did not take whole: written again
	long civ_rewrites_ = 0;
	// civ: a TX on/off the port did not take whole (EAGAIN with its buffer full, a short write, an
	// error) was lost without a word: a lost "TX off" leaves the radio keyed, deaf to the next packet
	// until the stuck-key check (0.3-0.8 s later). Written again at the next period if it is still
	// what we want (a frame cut short is resynchronised by the next FE FE). PTT_CIV_RETRY_OFF=1: not.
	void civ_retry(bool want_on)
	{
		if (civ_pend_ < 0) return;
		const bool on = civ_pend_ != 0;
		civ_pend_ = -1;
		if (on != want_on || on != keyed_ || port_.fd() < 0) return;   // no longer wanted: the change writes its own
		civ_rewrites_++;
		if (civ_rewrites_ % 20 == 1)
			std::fprintf(stderr, "[ptt] CI-V TX %s not taken whole by the port: written again (%ld time(s))\n", on ? "on" : "off", civ_rewrites_);
		civ_tx(on);
	}

	// rigctld's answers ("RPRT n" to a set, "0"/"1" to "t"), read without blocking; a "1" marks the
	// radio keyed (looked at only while we are not keying)
	void rig_drain()
	{
		char d[256];
		ssize_t n;
		while ((n = ::recv(fd_, d, sizeof d, MSG_DONTWAIT)) > 0)
			for (ssize_t i = 0; i < n; i++) {
				if (d[i] != '\n') { if (line_n_ < int(sizeof line_) - 1) line_[line_n_++] = d[i]; continue; }
				line_[line_n_] = 0;
				if (line_n_ == 1 && line_[0] == '1') rig_stuck_ = true;
				// a frequency (the answer to "f": Hz, digits only; "t" answers one digit, a set "RPRT n")
				else if (line_n_ >= 4 && std::strspn(line_, "0123456789") == size_t(line_n_)) {
					freq_hz_ = std::atof(line_); freq_t_ = last_now_;
					if (freq_wait_ > 0) freq_wait_--;
				}
				else if (freq_wait_ > 0 && std::strncmp(line_, "RPRT -", 6) == 0) freq_wait_--;   // the radio did not answer it
				line_n_ = 0;
			}
	}
	void rig_check(double now)
	{
		last_now_ = now;
		rig_drain();
		if (ext_tx_ || now - t_unkey_ < 0.3) { rig_stuck_ = false; return; }
		if (rig_stuck_) {
			rig_stuck_ = false;
			(void)!::send(fd_, "T 0\n", 4, MSG_DONTWAIT | MSG_NOSIGNAL);
			if (rig_unkeys_++ % 20 == 0)
				std::fprintf(stderr, "[ptt] the radio was still keyed after our unkey: \"T 0\" again (%ld time(s); a second keying path, e.g. the interface's VOX PTT, holds it)\n", rig_unkeys_);
		}
		if (now - t_poll_ >= 0.5) {
			t_poll_ = now;
			// every 2 s in standby, 2 s or more after the last unkey: the frequency instead of the PTT state (at most
			// one read in flight; one unanswered for 5 s is given up)
			if (freq_wait_ > 0 && now - freq_ask_ > 5.0) freq_wait_ = 0;
			if (freq_on_ && now - t_unkey_ >= 2.0 && now - freq_ask_ >= 2.0 && freq_wait_ == 0) {
				freq_ask_ = now; freq_wait_ = 1;
				(void)!::send(fd_, "f\n", 2, MSG_DONTWAIT | MSG_NOSIGNAL);
			} else (void)!::send(fd_, "t\n", 2, MSG_DONTWAIT | MSG_NOSIGNAL);
		}
	}
	double last_now_ = 0;

	// civ: the radio's answers, read without blocking. Only "FE FE E0 ADDR 1C 00 01 FD" (the radio
	// to us: TX is on) marks it keyed; our own commands echoed back (to ADDR), FB/FA answers and
	// transceive broadcasts are skipped.
	void civ_drain()
	{
		uint8_t d[128];
		ssize_t n;
		while ((n = port_.read(d, sizeof d)) > 0)
			for (ssize_t i = 0; i < n; i++) {
				const uint8_t b = d[i];
				if (b == 0xFE) {                             // a frame starts with FE FE (more FE: preamble)
					if (civ_n_ == 0 || civ_[civ_n_ - 1] != 0xFE) civ_n_ = 0;
					else if (civ_n_ >= 2) continue;
				}
				if (civ_n_ < int(sizeof civ_)) civ_[civ_n_++] = b;
				if (b != 0xFD) continue;
				if (civ_n_ == 8 && civ_[0] == 0xFE && civ_[1] == 0xFE && civ_[2] == 0xE0 && civ_[3] == spec_.civ_addr &&
				    civ_[4] == 0x1C && civ_[5] == 0x00 && civ_[6] == 0x01)
					rig_stuck_ = true;
				civ_n_ = 0;
			}
	}
	void civ_check(double now)
	{
		civ_drain();
		if (now - t_unkey_ < 0.3) { rig_stuck_ = false; return; }
		if (rig_stuck_) {
			rig_stuck_ = false;
			civ_tx(false);
			if (rig_unkeys_++ % 20 == 0)
				std::fprintf(stderr, "[ptt] the radio was still keyed after our unkey: CI-V TX off again (%ld time(s); a second keying path, e.g. the interface's VOX PTT, holds it)\n", rig_unkeys_);
		}
		if (now - t_poll_ >= 0.5) {
			t_poll_ = now;
			const uint8_t q[] = {0xFE, 0xFE, spec_.civ_addr, 0xE0, 0x1C, 0x00, 0xFD};
			(void)!port_.write(q, sizeof q);
		}
	}

	void raw_key(bool on)
	{
		if (spec_.kind == PttKind::RIG ? fd_ < 0 : port_.fd() < 0) return;
		switch (spec_.kind) {
		case PttKind::RTS: if (on) port_.lines(TIOCM_RTS, true); else port_.low(); break;
		case PttKind::DTR: if (on) port_.lines(TIOCM_DTR, true); else port_.low(); break;
		case PttKind::CIV: civ_drain(); rig_stuck_ = false; civ_tx(on); return;
		case PttKind::RIG: {                             // non-blocking; rigctld's answers are drained first
			rig_drain(); rig_stuck_ = false;
			(void)!::send(fd_, on ? "T 1\n" : "T 0\n", 4, MSG_DONTWAIT | MSG_NOSIGNAL);
			return;
		}
		default: return;
		}
		if (!on && spec_.civ_addr) civ_tx(false);         // belt and braces on an Icom
	}
	void civ_tx(bool on)
	{
		if (!spec_.civ_addr) return;
		const uint8_t cmd[] = {0xFE, 0xFE, spec_.civ_addr, 0xE0, 0x1C, 0x00, uint8_t(on ? 1 : 0), 0xFD};
		static const bool no_retry = std::getenv("PTT_CIV_RETRY_OFF") != nullptr;
		const bool whole = port_.write(cmd, sizeof cmd) == ssize_t(sizeof cmd);   // (no output here: signal handlers call this)
		civ_pend_ = whole || no_retry ? -1 : int(on);
	}
	// civ: does the radio answer at the port's rate? Asks the TX state (1C 00) up to 3 times, 0.2 s each,
	// for any frame from ADDR to us (the open only: it blocks). Several tries: another program holding
	// the port (the bench's KeyGuard) can read our answer first; one try missed 1 of 20 opens on the
	// bench, 2026-11-15.
	bool civ_answers()
	{
		uint8_t d[64];
		const uint8_t q[] = {0xFE, 0xFE, spec_.civ_addr, 0xE0, 0x1C, 0x00, 0xFD};
		for (int t = 0; t < 3; t++) {
			while (port_.read(d, sizeof d) > 0) {}
			if (port_.write(q, sizeof q) != ssize_t(sizeof q)) return false;
			int st = 0;                              // 0-3: FE FE E0 ADDR matched so far
			for (int i = 0; i < 20; i++) {
				ssize_t n;
				while ((n = port_.read(d, sizeof d)) > 0)
					for (ssize_t k = 0; k < n; k++) {
						const uint8_t b = d[k];
						if (st < 2) st = b == 0xFE ? st + 1 : 0;
						else if (st == 2) st = b == 0xE0 ? 3 : b == 0xFE ? 2 : 0;
						else if (b == spec_.civ_addr) return true;
						else st = b == 0xFE ? 1 : 0;
					}
				::usleep(10000);
			}
		}
		return false;
	}
};

}  // namespace radio

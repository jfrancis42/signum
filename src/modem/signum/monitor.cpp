// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// P2Monitor member functions (out of monitor.hpp, so that they build in parallel).
#include "monitor.hpp"

namespace fmodem {

void P2Monitor::tick(int16_t x)
{
	rx_.tick(x);                                    // ST_IDLE: stores the sample, answers nothing
	rx3_.tick(x);                                   // the same, as a P3 station (SL1 and P3 break-in decoders)
	for (auto &p : p1rx_) p.tick(x);               // P1 links: one passive decoder per sender (its text state)
	const int64_t n = rx_.n_;
	if (phase_ == P3DATA) p3_bursts();
	if (phase_ == SEARCH && n % 4 == 0) search_call();
	if (phase_ == SEARCH && !std::getenv("MON_NO_JOIN")) join_bursts();
	while (!ev_.empty() && ev_.front().at <= n) { const Ev e = ev_.front(); ev_.pop_front(); handle(e); }
}

void P2Monitor::text_out(int who, const std::string &t)
{
	for (char c : t) {
		if (c == '\r' || c == '\n') { if (!pending_[who].empty()) out_.push_back({T(rx_.n_), who, pending_[who]}); pending_[who].clear(); }
		else pending_[who] += c;
		if (pending_[who].size() >= 200) { out_.push_back({T(rx_.n_), who, pending_[who]}); pending_[who].clear(); }
	}
}

void P2Monitor::reset()
{
	if (rx_.n_ > 0) join_after_ = rx_.n_ + S(10.0);
	b1_at_ = -1;     // the ended QSO's last packets (QRT repeats) are not a new one to join
	phase_ = SEARCH; ev_.clear(); lat_on_ = false; mq3m_.clear(); caller_.clear(); callee_.clear(); misses_ = 0; qrt_seen_ = false; after_qrt_ = 0; last_bk_ = -1000000;
	last_cnt_[0] = last_cnt_[1] = -1; rle_[0] = rle_[1] = text::RleExpander(); long_ = false; level_ = 2; slow_ = setup_100_ = setup_noted_ = false;
}

bool P2Monitor::call_at(double t, bool inv, std::string &who) const
{
	uint8_t bits[72], bytes[9];
	rx_.bits_at(t, 100, 72, inv, bits);
	P2Station::pack(bits, 72, bytes);
	if (bytes[0] != 0x55) return false;
	std::string c;
	int i = 1;
	for (; i < 9 && call_char(bytes[i]); i++) c += char(bytes[i]);
	for (; i < 9; i++) if (bytes[i] != 0x0f) return false;
	if (c.size() < 3) return false;
	// a callsign has a digit and a letter (noise made "RUUUUUUU" once)
	if (c.find_first_of("0123456789") == std::string::npos || c.find_first_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ") == std::string::npos) return false;
	who = c;
	return true;
}

void P2Monitor::search_call()
{
	double t = double(rx_.n_) - rx_.gd_ - 72 * 96.0 - 48 * 48.0 - 2 - 60;
	if (t < 1) return;
	uint8_t b8[8], first;
	rx_.bits_at(t, 100, 8, false, b8);
	P2Station::pack(b8, 8, &first);
	if (first != 0x55 && first != 0xAA) return;
	for (int inv = 0; inv < 2; inv++) {
		std::string who;
		if (!call_at(t, inv, who)) continue;
		int64_t lo = int64_t(t), hi = int64_t(t);
		std::string w2;
		while (hi - int64_t(t) < 90 && call_at(double(hi + 1), inv, w2) && w2 == who) hi++;
		while (int64_t(t) - lo < 90 && call_at(double(lo - 1), inv, w2) && w2 == who) lo--;
		call_first_ = (lo + hi) / 2;
		callee_ = who;
		phase_ = CALL;
		dbg("call to " + callee_);                     // reported once answered (noise makes a "call" now and then)
		at(call_first_ + S(1.36), K_CS, call_first_);
		return;
	}
}

void P2Monitor::handle(const Ev &e)
{
	switch (e.k) {
	case K_CS: on_cs(e.ref); break;
	case K_SETUP: on_setup(e.ref); break;
	case K_ACCEPT: on_accept(e.ref); break;
	case K_PACKET: on_packet(e.ref, e.arg); break;
	case K_REPLY: on_reply(e.ref); break;
	case K_BKDATA: on_bk_data(e.ref); break;
	case K_P1PKT: on_p1_packet(e.ref); break;
	case K_P1CS: on_p1_cs(e.ref); break;
	case K_P1BK: on_p1_bk(e.ref); break;
	case K_P3LAT: on_p3_lattice(e.ref, e.arg); break;
	case K_P3ACQ: on_p3_acquire(e.ref); break;
	case K_P3B1: on_p3_b1(e.ref, e.arg); break;
	}
}

void P2Monitor::on_cs(int64_t ref)
{
	uint16_t code = 0; int64_t when = 0;
	const double sc = rx_.find_p1_code(ref + S(0.90), ref + S(1.35), {p2arq::kP1Cs1, p2arq::kP1Cs2, p2arq::kP1Cs4}, code, when);
	if (sc < 0.75) { dbg("no answer to the call"); phase_ = SEARCH; return; }   // the caller calls again: found again
	if (code == p2arq::kP1Cs2) { note("call to " + callee_ + ": busy (CS2)"); phase_ = SEARCH; return; }
	note("call to " + callee_ + " answered");
	slow_ = code == p2arq::kP1Cs4;                     // the set-up packet then comes at 100 Bd, P2 starts at level 1 (fix 43)
	if (slow_) note(callee_ + " answered at 100 Bd (CS4)");
	phase_ = SETUP; setup_tries_ = 0;
	at(ref + S(1.25 + 0.96 + 0.03), K_SETUP, ref + S(1.25));
}

bool P2Monitor::setup100_at(int64_t exp, int64_t &t_out, uint8_t *bytes)
{
	uint8_t bits[96];
	for (int64_t t = exp - S(0.012); t <= exp + S(0.012); t += 2)
		for (int inv = 0; inv < 2; inv++) {
			rx_.bits_at(double(t), 100, 96, inv, bits);
			P2Station::pack(bits, 96, bytes);
			if (bytes[0] != 0x55 && bytes[0] != 0xAA) continue;
			const uint16_t crc = uint16_t(bytes[10] | bytes[11] << 8);
			if (crc16_ccitt_reflected(bytes + 1, 9) == crc) { t_out = t; setup_100_ = true; return true; }
		}
	return false;
}

void P2Monitor::on_setup(int64_t exp)
{
	uint8_t bytes[24];
	int64_t t1 = 0;
	if (!rx_.p1_setup_at(exp, t1, bytes) && !(slow_ && setup100_at(exp, t1, bytes))) {
		if (++setup_tries_ > 5) { note("no set-up packet"); reset(); return; }
		at(exp + S(1.25 + 0.96 + 0.03), K_SETUP, exp + S(1.25));
		return;
	}
	setup_first_ = t1;
	if (slow_ && setup_100_) bytes[21] = bytes[9];      // the status byte of a 100 Bd packet (8 data bytes)
	const std::string body = text::decode((bytes[21] >> 2) & 3, bytes + 1, slow_ && setup_100_ ? 8 : 20);
	std::string who = body.substr(0, body.find('\r'));
	for (auto &ch : who) ch = char(std::toupper(uint8_t(ch)));
	if (!who.empty() && std::isdigit(uint8_t(who[0]))) who.erase(0, 1);
	caller_ = who.empty() ? "caller" : who;
	if (!setup_noted_) note(caller_ + " calls " + callee_ + " (offers " + std::string(bytes[21] & 0x20 ? "P3" : bytes[21] & 0x10 ? "P2" : "P1 only") + ")");   // once, not per repeat
	setup_noted_ = true;
	phase_ = ACCEPT;
	at(t1 + S(1.36), K_ACCEPT, t1);
}

void P2Monitor::on_accept(int64_t t1)
{
	uint16_t code = 0; int64_t when = 0;
	const double sc = rx_.find_p1_code(t1 + S(0.90), t1 + S(1.35), {p2arq::kP2Accept, p3::kP3Accept, p2arq::kP1Cs1, p2arq::kP1Cs2, p2arq::kP1Cs4}, code, when);
	// After a CS4 call answer the link runs at 100 Bd: CS4 to the set-up
	// packet asks for it again (CS4s in a row are requests [spec]) and CS1
	// acknowledges it (a P1 link); at 200 Bd CS1 asks again.
	const bool again = slow_ ? code == p2arq::kP1Cs4 : code == p2arq::kP1Cs1;
	if (slow_ && code == p2arq::kP1Cs1) code = p2arq::kP1Cs2;   // the P1 acknowledgement, below
	if (sc < 0.75 || again) {                           // not heard / asked again: the set-up packet repeats
		if (++setup_tries_ > 5) { note("set-up not answered"); reset(); return; }
		phase_ = SETUP;
		at(t1 + S(1.25 + 0.96 + 0.03), K_SETUP, t1 + S(1.25));
		return;
	}
	if (code == p3::kP3Accept) {
		note("P3 link " + caller_ + " - " + callee_);
		phase_ = P3DATA; p3_anchor_ = t1 - S(0.0225) + S(1.286); p3_sender_ = 0; p3_phase_ = -1.0;
		at(p3_anchor_ + S(1.25 + 0.885 + 0.92), K_P3ACQ, p3_anchor_ + S(1.25));
		at(p3_anchor_ + S(0.885 + 0.92), K_P3B1, p3_anchor_ + S(0.885), 0);
		b_on_ = false; nf_ = -1; quiet_ = 0; last_cnt_[0] = last_cnt_[1] = -1;
		return;
	}
	if (code == p2arq::kP1Cs2 || code == p2arq::kP1Cs4) {
		// The set-up packet was the caller's first P1 packet (counter 1,
		// "<level><call>\r" and its first text): the callee acknowledged it.
		note("P1 link " + caller_ + " - " + callee_);
		phase_ = P1DATA; p1_iss_ = 0; misses_ = 0; p1_last_cnt_[0] = 1; p1_last_cnt_[1] = -1;
		for (auto &p : p1rx_) { p.ident_pending_ = false; p.esc_pending_ = false; p.rx_.clear(); }
		uint8_t bytes[24]; int64_t t2 = 0;
		if (rx_.p1_setup_at(t1, t2, bytes)) {
			const std::string body = text::decode((bytes[21] >> 2) & 3, bytes + 1, 20);
			const size_t cr = body.find('\r');
			if (cr != std::string::npos) p1_text(0, body.substr(cr + 1));
		}
		p1_slot_ = t1 + S(1.25);
		at(p1_slot_ + S(1.0), K_P1PKT, p1_slot_);
		return;
	}
	note("P2 link " + caller_ + " - " + callee_ + (slow_ ? " (from level 1)" : ""));
	phase_ = P2DATA; iss_ = 0; level_ = slow_ ? 1 : 2; long_ = false; misses_ = 0;
	slot_ = t1 - S(0.0225) + S(1.286);                // the caller's first PSK packet (as our callee expects it)
	at(slot_ + S(0.86), K_PACKET, slot_, 0);
}

void P2Monitor::p1_deliver(int who, const P1Station::RxPacket &p)
{
	const int cnt = p.status & 3;
	if (!p.bk && cnt == p1_last_cnt_[who]) return;   // a repeat
	if (!p.bk && p1_last_cnt_[who] >= 0 && cnt != ((p1_last_cnt_[who] + 1) & 3) &&
	    (pending_[who].size() < 7 || pending_[who].compare(pending_[who].size() - 7, 7, " [...] ") != 0)) pending_[who] += " [...] ";   // packets we did not read (once per gap)
	p1_last_cnt_[who] = p.bk ? 0 : cnt;
	p1rx_[who].deliver(p);
	p1_text(who, p1rx_[who].take_rx());
}

void P2Monitor::on_p1_packet(int64_t slot)
{
	P1Station::RxPacket p;
	if (p1rx_[0].find_packet(slot - S(0.010), slot + S(0.010), p, false)) {
		misses_ = 0; p1_fails_ = 0;
		if (p.status & 0x80) { p1_end("QRT"); return; }
		p1_deliver(p1_iss_, p);
		dbg("P1 packet from " + call(p1_iss_) + " " + std::to_string(p.baud) + " Bd status " + std::to_string(p.status));
	} else {
		if (++misses_ > 30) { p1_end("lost"); return; }
		// A break-in we did not read: the other station's packets come on its
		// lattice, its reply time after ours less a cycle. Tried after two
		// failures here.
		if (++p1_fails_ >= 2) {
			const int64_t alt = slot + S(p1_reply_) - S(1.25);
			// +-30 ms: the firmware's packets after its break-in came 16-17 ms
			// later than its CS replies predict (emulated pair, 2026-10-20)
			if (p1rx_[0].find_packet(alt - S(0.030), alt + S(0.030), p, false)) {
				const int other = 1 - p1_iss_;
				flush(p1_iss_);
				note(call(other) + " is sending (its break-in not read)");
				p1_iss_ = other; p1_fails_ = 0; misses_ = 0; p1_last_cnt_[other] = -1;
				if (p.status & 0x80) { p1_end("QRT"); return; }
				p1_deliver(other, p);
				p1_slot_ = p.first;
				at(p1_slot_ + S(1.24), K_P1CS, p1_slot_);
				return;
			}
		}
	}
	at(slot + S(1.24), K_P1CS, slot);
}

void P2Monitor::on_p1_cs(int64_t slot)
{
	Cs cs = CS_NONE; int64_t when = 0; bool inv = false;
	const double sc = p1rx_[0].find_cs(slot + S(0.90), slot + S(1.24), cs, when, inv);
	if (sc >= 0.95 && cs != CS3) p1_reply_ += 0.2 * (double(when - slot) / rx_.cfg_.fs - p1_reply_);
	if (sc >= 0.75 && cs == CS3) {                     // break-in: its packet is in 0.96 s after the CS3
		at(when + S(0.96 + 0.05), K_P1BK, when);
		return;
	}
	p1_slot_ = slot + S(1.25);
	at(p1_slot_ + S(1.0), K_P1PKT, p1_slot_);
}

void P2Monitor::on_p1_bk(int64_t when)
{
	P1Station::RxPacket p;
	const int brk = 1 - p1_iss_;
	if (p1rx_[0].find_packet(when - S(0.005), when + S(0.005), p, true) && p.bk) {
		flush(p1_iss_);
		note(call(brk) + " breaks in");
		p1_iss_ = brk; p1_last_cnt_[brk] = -1;
		p1_deliver(brk, p);
		p1_slot_ = when + S(1.25);                   // the new sender's packets
	} else p1_slot_ = p1_slot_ + S(1.25);              // not read: the old lattice
	at(p1_slot_ + S(1.0), K_P1PKT, p1_slot_);
}

void P2Monitor::on_packet(int64_t slot, int mode)
{
	if (phase_ != P2DATA) return;
	// Still on 0.86 s in: a long packet, or the receiver's reply already
	// starting (the monitor hears both stations; long packets came here
	// without an R seen, so fix 55's rule does not settle it). Decided at
	// +1.2 s: a control signal or break-in at the reply time means the
	// packet was short (a CS4 link: every break-in had been read 2.5 s late).
	if (mode == 0) {
		// against the packet's own body: the gap before it may hold the other
		// station's reply (the monitor hears both)
		const double late = rx_.window_power(slot + S(0.835), slot + S(0.858)), body = rx_.window_power(slot + S(0.2), slot + S(0.6));
		if (late > 0.25 * body) { at(slot + S(1.20), K_PACKET, slot, 2); return; }
	}
	if (mode == 2) {
		const int64_t r = slot + S(iss_ == 0 ? 0.879 : 0.971);
		const P2Station::ReplyRx rr = rx_.classify_reply(r, true, nullptr, rx_.afc_);
		const bool reply = (rr.bk && rr.start > double(r) - double(S(0.3))) || (rr.cs >= 0 && rr.score >= 0.75);
		if (!reply) { at(slot + S(3.36), K_PACKET, slot, 1); return; }
		dbg("not a long packet: the reply is there");
	}
	const bool waited = mode == 1;
	rx_.level_ = level_;
	rx_.trk_clear();
	P2Station::RxPacket q = rx_.find_packet(slot, false, waited);
	// The receiver's S / T moved the level (we follow the letters loosely):
	// the neighbouring levels before giving the packet up.
	for (int dl : {1, -1}) {
		if (q.ok || level_ + dl < 1 || level_ + dl > 4) continue;
		rx_.level_ = level_ + dl;
		P2Station::RxPacket q2 = rx_.find_packet(slot, false, waited);
		if (q2.ok) q = q2;
	}
	rx_.level_ = level_;
	const bool lng = q.ok ? q.lng : waited;
	q = rx_.marq(q, slot);                             // memory ARQ: the sender repeats a packet its receiver did not take
	if (q.ok) {
		misses_ = 0; level_ = q.level; long_ = q.lng;
		const int nd = p2::data_bytes(q.level, q.lng ? 40 : 9);
		const int status = q.info.size() > size_t(nd) ? q.info[size_t(nd)] : 0;
		deliver(iss_, q.info.data(), nd, status);
		if ((status & 0x80) && !qrt_seen_) { note("QRT from " + call(iss_)); qrt_seen_ = true; }
		slot_ = int64_t(std::llround(q.start));        // follow the sender's lattice
		if (debug_) {
			char h[64]; std::snprintf(h, sizeof h, " hdr %.2f metric %d bytes %02x %02x %02x %02x", q.hdr_rms, q.metric, q.info[0], q.info[1], q.info[2], q.info[3]);
			dbg(call(iss_) + " packet L" + std::to_string(q.level) + (q.lng ? " long" : "") + " status " + std::to_string(status) + h);
		}
	} else {
		dbg(call(iss_) + " packet not decoded (slot " + std::to_string(T(slot)) + ")");
		slot_ = slot;                                  // the lattice goes on from this slot (from the last good one, every later time was in the past)
		if (qrt_seen_ && ++after_qrt_ > 3) { note("end of the QSO"); reset(); return; }
		if (++misses_ > 25) { note("signal lost: end of the QSO"); reset(); return; }
	}
	// the receiver's control signal (or break-in): the callee answers 0.879 s
	// into the caller's slot, the caller 0.971 s into the callee's
	const int64_t r = slot_ + S((lng ? 2.5 : 0.0) + (iss_ == 0 ? 0.879 : 0.971));
	at(r + S(0.34), K_REPLY, r);
	pending_slot_ = slot_ + S(lng ? 3.75 : 1.25);
}

void P2Monitor::on_reply(int64_t exp)
{
	if (phase_ != P2DATA) return;
	const P2Station::ReplyRx r = rx_.classify_reply(exp, true, nullptr, rx_.afc_);
	// a break-in starts after the packet it answers, and is taken once (the
	// same pattern found again from a later look made the roles flip for
	// ever at the end of a QSO)
	if (r.bk && r.start > double(exp) - double(S(0.3)) && r.start > double(last_bk_) + double(S(0.6))) {
		last_bk_ = int64_t(r.start);
		dbg(call(1 - iss_) + " breaks in");
		at(int64_t(r.start) + S(0.90), K_BKDATA, int64_t(r.start));
		return;
	}
	const int cs = r.cs >= 0 && r.score >= 0.75 ? r.cs : r.soft_cs;
	if (debug_ && cs >= 0) dbg(call(1 - iss_) + " " + std::string(1, p2arq::kCsName[cs]));
	// Memory ARQ only across copies of one packet: the same P/Q as last time
	// asks for a repeat; any other letter (or none heard) means the sender
	// moves on, so the sum starts again. Summed across different packets a
	// sum once passed its CRC with garbage (noise 2000).
	const bool repeat = (cs == p2arq::CS_P || cs == p2arq::CS_Q) && cs == last_cs_;
	if (!repeat) rx_.marq_clear();
	last_cs_ = cs;
	if (qrt_seen_ && ++after_qrt_ > 3) { note("end of the QSO"); reset(); return; }
	at(pending_slot_ + S(0.86), K_PACKET, pending_slot_, 0);
}

void P2Monitor::on_bk_data(int64_t bk)
{
	if (phase_ != P2DATA) return;
	const int other = 1 - iss_;
	P2Station::RxPacket p = rx_.find_packet(bk, true, false);
	if (p.ok) {
		const int n = p2::data_bytes(p.level, 7);
		const int status = p.info.size() > size_t(n) ? p.info[size_t(n)] : -1;
		deliver(other, p.info.data(), n, status);
		level_ = p.level;
		if (status >= 0 && (status & 0x80) && !qrt_seen_) { note("QRT from " + call(other)); qrt_seen_ = true; }   // a QRT carried by the break-in
		if (debug_) {
			char h[48]; std::snprintf(h, sizeof h, "break-in L%d status %d bytes", p.level, status);
			std::string hx = h; for (int i = 0; i < n && i < 24; i++) { char t[4]; std::snprintf(t, sizeof t, " %02x", p.info[size_t(i)]); hx += t; }
			dbg(hx);
		}
	} else dbg("break-in data not decoded");
	iss_ = other; last_cnt_[iss_] = -1; long_ = false;
	rx_.marq_clear(); last_cs_ = -1;                   // copies from the other station
	// The old sender's next look is still queued: drop it (it ran as the new
	// sender's on the old lattice and started a second chain of looks).
	for (auto it = ev_.begin(); it != ev_.end();) it = (it->k == K_PACKET || it->k == K_REPLY) ? ev_.erase(it) : it + 1;
	note(call(iss_) + " sends");
	const int64_t next = bk + S(1.25);
	dbg("break-in at " + std::to_string(T(bk)) + ", next packet " + std::to_string(T(next)) + ", now " + std::to_string(T(rx_.n_)));
	at(next + S(0.86), K_PACKET, next, 0);
}

void P2Monitor::p3_bursts()
{
	const int64_t n = rx_.n_, F = S(0.010);
	if (n % F) return;
	const double p = rx_.window_power(n - F, n);
	// the floor follows quiet frames; at least rms 100 (radio audio always has
	// more noise; digital silence let pulse tails join the bursts)
	if (nf_ < 0) nf_ = std::max(p, 1e4);
	nf_ = std::max(1e4, p < nf_ ? 0.9 * nf_ + 0.1 * p : std::min(nf_ * 1.002, p));
	const bool on = p > 1.8 * nf_;                     // ~5 sigma over the floor (96-sample frames); 6x missed every burst at noise 2000 (signal ~ noise)
	if (debug_ && std::getenv("MON_PWR") && n % S(0.25) == 0) dbg("p " + std::to_string(int(p)) + " nf " + std::to_string(int(nf_)) + (on ? " on" : ""));
	if (on) { if (!b_on_) { b_on_ = true; b_start_ = n - F; } b_last_ = n; quiet_ = 0; }
	else if (b_on_ && n - b_last_ > S(0.05)) { b_on_ = false; p3_burst(b_start_, b_last_); }
	else if (!b_on_ && ++quiet_ > 1000) { note("signal lost: end of the QSO"); reset(); }   // 10 s of nothing
}

void P2Monitor::p3_burst(int64_t s, int64_t e)
{
	const double dur = T(e - s);
	if (dur < 0.5) return;                             // control signals
	if (lat_on_) {                                     // the sender's lattice looks at these itself
		const double d = std::fabs(std::remainder(T(s + S(0.03) - lat_next_), 1.25));
		if (d < 0.2) return;
	}
	const bool lng = dur > 2.9 && dur < 3.8;           // a long packet is ~3.3 s; 1-2.6 s is two short bursts run together
	const bool a = p3_burst_at(s, e, lng);
	const double plen = lng ? 3.33 : 0.86;
	if (dur > plen + 0.15 && (!a || dur > 2 * plen - 0.1)) p3_burst_at(e - S(plen), e, lng);
}

bool P2Monitor::p3_burst_at(int64_t s, int64_t e, bool long_burst)
{
	const double dur = long_burst ? 2.0 : 1.0;
	const int64_t a = s - S(0.06);
	// at least a whole packet from the burst's start: in noise the energy
	// edge ends early and cut the packet's tail off (noise 1500: 60 % of
	// SL3 packets lost that the firmware's receiver took)
	const auto x = window(a, std::max(e, s + S(long_burst ? 3.33 : 0.86)) + S(0.06));
	const double fs = rx_.cfg_.fs, t0 = double(s - a) + 0.030 * fs;
	int level = 0, status = -1; std::vector<uint8_t> data; bool lng = false;
	double t_pkt = t0;                                 // the decoded packet's start in x (its phase tells the sender)
	// the last level first (one try, mostly: the live loop has to keep up)
	// steady state: the last level and its neighbours only; every level (and
	// SL1 / break-ins) once in four failed bursts. Every level for every
	// failed burst overran even a 2 s capture buffer live.
	int order[5] = {2, 3, 4, 5, 6}, nlv = 5;
	if (lvl_hint_ >= 2 && lvl_hint_ <= 6) {
		int k = 0; order[k++] = lvl_hint_;
		if (lvl_hint_ > 2) order[k++] = lvl_hint_ - 1;
		if (lvl_hint_ < 6) order[k++] = lvl_hint_ + 1;
		if (fails_ % 4 == 3) for (int lv = 2; lv <= 6; lv++) if (lv != lvl_hint_ && lv != lvl_hint_ - 1 && lv != lvl_hint_ + 1) order[k++] = lv;
		nlv = k;
	}
	const bool full = lvl_hint_ < 2 || fails_ % 4 == 3;
	const double ph_b = std::fmod(T(s + S(0.030) - p3_anchor_) + 100 * 1.25, 1.25);
	if (dur > 1.5) {                                   // a long packet
		for (int oi5 = 0; oi5 < nlv && !level; oi5++) {
			const int lv = order[oi5];
			const p3::LvSync sy = p3::sync_level(lv, x.data(), long(x.size()), fs, t0, 25.0);
			for (int oi = 0; oi < 2 && !level; oi++) {
				const p3::LongRx r = long_from_products(lv, long_products(lv, x, sy.t, oi ? !sy.odd : sy.odd));
				if (r.ok) { level = lv; status = r.status; data = r.data; lng = true; t_pkt = sy.t; }
			}
		}
	} else {
		// Memory ARQ at the expected level (as the station, fix 86): a failed
		// copy on the current sender's phase is kept; the next failure is
		// decoded from the sum (both carrier maps), the counter checked.
		const bool on_phase = p3_phase_ >= 0 && std::fabs(std::remainder(ph_b - p3_phase_, 1.25)) < 0.15;
		for (int oi5 = 0; oi5 < nlv && !level; oi5++) {
			const int lv = order[oi5];
			const bool mq = lv == lvl_hint_ && on_phase && !std::getenv("MON_NO_MARQ");
			if (lv == 5) {
				std::vector<std::complex<double>> dd;
				const p3::RxPacket r = p3::finish_at(x.data(), long(x.size()), fs, p3::sync_at(x.data(), long(x.size()), fs, t0, 25.0), mq ? &dd : nullptr);
				if (r.ok) { level = 5; status = r.r.info[p3::kDataBytes]; data.assign(r.r.info, r.r.info + p3::level_data_bytes(5)); t_pkt = r.t0; }
				else if (mq && !dd.empty()) {
					for (const auto &c : mq3m_) if (c.level == 5) for (size_t w = 0; w < dd.size() && w < c.d0.size(); w++) dd[w] += c.d0[w];
					const p3::RxResult rr = mq3m_.empty() ? p3::RxResult{} : p3::sl5_decode_diffs(dd);
					if (rr.ok && mq_cnt_ok(rr.info[p3::kDataBytes])) { level = 5; status = rr.info[p3::kDataBytes]; data.assign(rr.info, rr.info + p3::level_data_bytes(5)); dbg("memory ARQ: SL5 from " + std::to_string(mq3m_.size() + 1) + " copies"); }
					else mq_keep(5, std::move(dd), {});
				}
			} else {
				std::vector<std::complex<double>> d0, d1;
				const p3::LvRx r = p3::finish_level(lv, x.data(), long(x.size()), fs, p3::sync_level(lv, x.data(), long(x.size()), fs, t0, 25.0), 0, mq ? &d0 : nullptr, mq ? &d1 : nullptr);
				if (r.ok) { level = lv; status = r.status; data = r.data; t_pkt = r.t0; }
				else if (mq && !d0.empty()) {
					bool got = false;
					for (int m0 = 0; m0 < 2 && !mq3m_.empty() && !got; m0++) {
						auto sum = m0 ? d1 : d0;
						for (size_t c = 0; c < mq3m_.size(); c++) {
							const bool odd = ((mq3m_.size() - c) & 1) ? !m0 : m0;   // the map alternates per transmission
							const auto &d = odd ? mq3m_[c].d1 : mq3m_[c].d0;
							for (size_t w = 0; w < sum.size() && w < d.size(); w++) sum[w] += d[w];
						}
						const p3::Lv23Rx rr = p3::decode_level_diffs(lv, sum);
						if (rr.ok && mq_cnt_ok(rr.status)) { level = lv; status = rr.status; data = rr.data; got = true; dbg("memory ARQ: SL" + std::to_string(lv) + " from " + std::to_string(mq3m_.size() + 1) + " copies"); }
					}
					if (!got) mq_keep(lv, std::move(d0), std::move(d1));
				}
			}
		}
		if (!level) {                                  // a P3 break-in (changeovers, QRTs: always tried) or SL1, through the P3 station
			P2Station::RxPacket q = rx3_.find_packet(s + S(0.012), true, false);
			if (q.ok && q.bk) { level = -1; status = q.info.back(); data.assign(q.info.begin(), q.info.end() - 1); t_pkt = q.start - double(a); }
			else if (full || lvl_hint_ <= 2) {
				rx3_.level_ = 1;
				q = rx3_.find_packet(s + S(0.012), false, false);
				if (q.ok && q.level == 1) { level = 1; status = q.info[size_t(p3::kSl1Data)]; data.assign(q.info.begin(), q.info.begin() + p3::kSl1Data); t_pkt = q.start - double(a); }
			}
		}
	}
	if (!level && lvl_hint_ < 2 && !retimed_ && !std::getenv("MON_NO_RETIME")) {   // no level at the energy edge: the packet may sit off it (a control signal ran into it)
		retimed_ = true;
		bool got = false;
		for (int off : {-40, 40, -80, 80}) if (!got) got = p3_burst_at(s + S(off / 1000.0), e + S(off / 1000.0), long_burst);
		retimed_ = false;
		if (got) return true;
	}
	if (!level) { fails_++; dbg("burst at " + std::to_string(T(s)).substr(0, 7) + " not decoded" + (std::getenv("MON_MQDBG") ? " ph " + std::to_string(ph_b) + " want " + std::to_string(p3_phase_) + " hint " + std::to_string(lvl_hint_) + " stored " + std::to_string(mq3m_.size()) : std::string())); return false; }
	fails_ = 0; mq3m_.clear(); last_dec_ = rx_.n_;
	if (level >= 2) lvl_hint_ = level;
	// whose: the sender keeps its phase on the 1.25 s cycle
	// (from the decoded start, not the energy edge: live audio ran bursts
	// together differently, and every other packet's phase moved)
	const double cyc = 1.25, ph = std::fmod(T(a + int64_t(t_pkt) - p3_anchor_) + 100 * cyc, cyc);
	if (p3_phase_ >= 0) {
		const double d = std::fabs(std::remainder(ph - p3_phase_, cyc));
		if (d > 0.12) { p3_sender_ ^= 1; last_cnt_[p3_sender_] = -1; mq3m_.clear(); lat_on_ = false; note(call(p3_sender_) + " sends"); }
	} else {
		// the first packet: the callee's B1 and greeting come 0.885 s into the
		// A1 cycle; the caller's A1 at 0 and its data after A2 at 0.885 + 0.971
		// = 0.606 (its greeting decoded first once, and every label was swapped)
		const double d1 = std::fabs(std::remainder(ph - 0.885, cyc));
		const double dc = std::min(std::fabs(std::remainder(ph, cyc)), std::fabs(std::remainder(ph - 0.606, cyc)));
		p3_sender_ = d1 < dc ? 1 : 0;
	}
	p3_phase_ = ph;
	if (debug_) dbg(call(p3_sender_) + (level < 0 ? std::string(" break-in") : " SL" + std::to_string(level) + (lng ? " long" : "")) + " status " + std::to_string(status));
	if (level < 0) {                                   // break-in data: ASCII, its pseudo status carries the type and QRT
		if (b1_at_ >= 0 && std::fabs(T(s - b1_at_)) < 0.4) { dbg("the callee's B1 again (taken by its timing)"); return true; }   // live: B1 is a burst too
		if (b1_at_ < 0) b1_at_ = s;                     // (the first break-in may be B1: its timed decode then skips it)
		text_out(p3_sender_, rle_[p3_sender_].feed(text::decode((status >> 2) & 7, data.data(), int(data.size()))));
		if (status & 0x80) note("QRT from " + call(p3_sender_));
		return true;
	}
	deliver(p3_sender_, data.data(), int(data.size()), status);
	if (status & 0x80) note("QRT from " + call(p3_sender_));
	if (!lng && level >= 1 && !std::getenv("MON_NO_LATTICE")) lat_start(a + int64_t(t_pkt) + S(1.25));
	else lat_on_ = false;
	return true;
}

void P2Monitor::on_p3_lattice(int64_t t, int gen)
{
	if (phase_ != P3DATA || !lat_on_ || gen != lat_gen_) return;
	const int64_t a = t - S(0.09);
	const auto x = window(a, t + S(0.86 + 0.06));
	int level = 0, status = -1; std::vector<uint8_t> data; double t_pkt = double(t - a);
	const int hint = lvl_hint_ >= 2 ? lvl_hint_ : 3;
	const int order[3] = {hint, hint > 2 ? hint - 1 : hint + 1, hint < 6 && hint > 2 ? hint + 1 : (hint == 2 ? 4 : 5)};
	for (int oi = 0; oi < 3 && !level; oi++) p3_decode_level(order[oi], x, double(t - a), 8.0, oi == 0, level, status, data, t_pkt);
	if (!level) {
		if (++lat_miss_ > 6) { lat_on_ = false; dbg("lattice dropped"); return; }   // back to the energy detector
		dbg("lattice " + std::to_string(T(t)).substr(0, 7) + " not decoded (stored " + std::to_string(mq3m_.size()) + ")");
		lat_next_ = t + S(1.25);
		at(lat_next_ + S(0.92), K_P3LAT, lat_next_, lat_gen_);
		return;
	}
	lat_miss_ = 0; mq3m_.clear(); fails_ = 0; last_dec_ = rx_.n_;
	if (level >= 2) lvl_hint_ = level;
	p3_phase_ = std::fmod(T(a + int64_t(t_pkt) - p3_anchor_) + 100 * 1.25, 1.25);
	if (debug_) dbg(call(p3_sender_) + " SL" + std::to_string(level) + " status " + std::to_string(status) + " (lattice)");
	deliver(p3_sender_, data.data(), int(data.size()), status);
	if (status & 0x80) note("QRT from " + call(p3_sender_));
	lat_next_ = a + int64_t(t_pkt) + S(1.25);
	at(lat_next_ + S(0.92), K_P3LAT, lat_next_, lat_gen_);
}

void P2Monitor::join_bursts()
{
	const int64_t n = rx_.n_, F = S(0.010);
	if (n % F) return;
	const double p = rx_.window_power(n - F, n);
	if (jnf_ < 0) jnf_ = std::max(p, 1e4);
	jnf_ = std::max(1e4, p < jnf_ ? 0.9 * jnf_ + 0.1 * p : std::min(jnf_ * 1.002, p));
	if (p > 1.8 * jnf_) { if (!jb_on_) { jb_on_ = true; jb_start_ = n - F; } jb_last_ = n; }
	else if (jb_on_ && n - jb_last_ > S(0.05)) {
		jb_on_ = false;
		const double dur = T(jb_last_ - jb_start_);
		if (n >= join_after_ && ((dur > 0.7 && dur < 1.15) || (dur > 2.9 && dur < 3.8))) try_join(jb_start_, jb_last_, dur > 2.9);
	}
}

void P2Monitor::try_join(int64_t s, int64_t e, bool lng)
{
	caller_ = "STN1"; callee_ = "STN2";
	// P3: the burst decoder at every level (it starts the lattice and the sender's phase)
	phase_ = P3DATA; p3_anchor_ = s + S(0.03); p3_sender_ = 0; p3_phase_ = -1.0; lvl_hint_ = 0; lat_on_ = false; fails_ = 0;
	last_cnt_[0] = last_cnt_[1] = -1;
	if (p3_burst_at(s, e, lng)) {
		note("joined a P3 QSO in progress (its calls were not heard: STN1, STN2)");
		at(p3_anchor_ + S(1.25 + 0.885 + 0.92), K_P3ACQ, p3_anchor_ + S(1.25));
		return;
	}
	phase_ = SEARCH;
	// P2: levels 1-4, short or long
	for (int lv = 1; lv <= 4; lv++) {
		rx_.level_ = lv;
		for (int64_t st : {s, s + S(0.012), s - S(0.012)}) {
			P2Station::RxPacket q = rx_.find_packet(st, false, lng);
			if (!q.ok || q.bk) continue;
			phase_ = P2DATA; iss_ = 0; level_ = q.level; long_ = q.lng; misses_ = 0; last_cs_ = -1;
			note("joined a P2 QSO in progress (its calls were not heard: STN1, STN2)");
			const int nd = p2::data_bytes(q.level, q.lng ? 40 : 9);
			deliver(0, q.info.data(), nd, q.info.size() > size_t(nd) ? q.info[size_t(nd)] : 0);
			slot_ = int64_t(q.start);
			pending_slot_ = slot_ + S(q.lng ? 3.75 : 1.25);
			at(pending_slot_ + S(0.86), K_PACKET, pending_slot_, 0);
			return;
		}
	}
	rx_.level_ = 2;
	// P1: 100 or 200 Bd
	P1Station::RxPacket p;
	if (!lng && p1rx_[0].find_packet(s - S(0.030), s + S(0.030), p, false)) {
		phase_ = P1DATA; p1_iss_ = 0; misses_ = 0; p1_fails_ = 0; p1_last_cnt_[0] = -1; p1_last_cnt_[1] = -1;
		note("joined a P1 QSO in progress (its calls were not heard: STN1, STN2)");
		if (p.status & 0x80) { p1_end("QRT"); return; }
		p1_deliver(0, p);
		p1_slot_ = p.first;
		at(p1_slot_ + S(1.24), K_P1CS, p1_slot_);
		return;
	}
	caller_.clear(); callee_.clear();
}

void P2Monitor::on_p3_b1(int64_t start, int tries)
{
	if (phase_ != P3DATA) return;
	if (b1_at_ >= 0 && std::fabs(T(start - b1_at_)) < 0.4) { dbg("callee B1 already taken as a burst"); return; }
	for (int off : {0, -10, 10, -20, 20, -30, 30}) {
		const P2Station::RxPacket q = rx3_.find_packet(start + S(off / 1000.0), true, false);
		if (q.ok && q.bk) {
			b1_at_ = start;
			deliver(1, q.info.data(), p3::kBkData, q.info[size_t(p3::kBkData)] & 0x1c);
			dbg("callee B1 decoded (" + std::to_string(off) + " ms)");
			return;
		}
	}
	dbg("callee B1 not decoded at " + std::to_string(T(start)));
	if (tries < 1) at(start + S(1.25 + 0.92), K_P3B1, start + S(1.25), tries + 1);
}

void P2Monitor::on_p3_acquire(int64_t cyc0)
{
	if (phase_ != P3DATA) return;
	at(cyc0 + S(1.25 + 0.885 + 0.92), K_P3ACQ, cyc0 + S(1.25));
	if (lat_on_ || rx_.n_ - last_dec_ < S(2.6) || std::getenv("MON_NO_ACQ")) { acq_left_ = 12; return; }
	if (acq_left_-- <= 0) {                            // 15 s of nothing on any candidate: the QSO is over
		if (acq_left_ == -1) { note("signal lost: end of the QSO"); reset(); }
		return;
	}
	// candidates: after a changeover the new sender's packets come its reply
	// offset after the old phase (callee 0.885 s, caller 0.971 s); the same
	// sender after a gap; and the set-up's phases
	std::vector<std::pair<int, double>> cand;
	if (p3_phase_ >= 0) {
		const int other = 1 - p3_sender_;
		cand.push_back({other, std::fmod(p3_phase_ + (other ? 0.885 : 0.971), 1.25)});
		cand.push_back({p3_sender_, p3_phase_});
	}
	cand.push_back({0, 0.606}); cand.push_back({1, 0.885});
	for (const auto &cw : cand) {
		const int who = cw.first;
		const double ph = cw.second > 0.885 + 1e-6 ? cw.second - 1.25 : cw.second;   // in this cycle, already in
		const int64_t t = cyc0 + S(ph), a = t - S(0.09);
		const auto x = window(a, t + S(0.86 + 0.06));
		int level = 0, status = -1; std::vector<uint8_t> data; double t_pkt = double(t - a);
		const int h = lvl_hint_ >= 2 ? lvl_hint_ : 3;
		for (int lv : {h, h > 2 ? h - 1 : 4, h < 6 ? h + 1 : 5}) { if (level) break; p3_decode_level(lv, x, double(t - a), 12.0, false, level, status, data, t_pkt); }
		if (!level) continue;
		if (who != p3_sender_ && p3_phase_ >= 0) { last_cnt_[who] = -1; note(call(who) + " sends"); }
		p3_sender_ = who; lvl_hint_ = level >= 2 ? level : lvl_hint_;
		p3_phase_ = std::fmod(T(a + int64_t(t_pkt) - p3_anchor_) + 100 * 1.25, 1.25);
		if (debug_) dbg(call(who) + " SL" + std::to_string(level) + " status " + std::to_string(status) + " (acquired on the set-up lattice)");
		deliver(who, data.data(), int(data.size()), status);
		if (status & 0x80) note("QRT from " + call(who));
		last_dec_ = rx_.n_;
		lat_start(a + int64_t(t_pkt) + S(1.25));
		return;
	}
}

void P2Monitor::p3_decode_level(int lv, const std::vector<float> &x, double t0, double search_ms, bool mq, int &level, int &status, std::vector<uint8_t> &data, double &t_pkt)
{
	const double fs = rx_.cfg_.fs;
	mq = mq && !std::getenv("MON_NO_MARQ");
	if (lv == 5) {
		std::vector<std::complex<double>> dd;
		const p3::RxPacket r = p3::finish_at(x.data(), long(x.size()), fs, p3::sync_at(x.data(), long(x.size()), fs, t0, search_ms), mq ? &dd : nullptr);
		if (r.ok) { level = 5; status = r.r.info[p3::kDataBytes]; data.assign(r.r.info, r.r.info + p3::level_data_bytes(5)); t_pkt = r.t0; return; }
		if (!mq || dd.empty()) return;
		for (const auto &c : mq3m_) if (c.level == 5) for (size_t w = 0; w < dd.size() && w < c.d0.size(); w++) dd[w] += c.d0[w];
		const p3::RxResult rr = mq3m_.empty() ? p3::RxResult{} : p3::sl5_decode_diffs(dd);
		if (rr.ok && mq_cnt_ok(rr.info[p3::kDataBytes])) { level = 5; status = rr.info[p3::kDataBytes]; data.assign(rr.info, rr.info + p3::level_data_bytes(5)); dbg("memory ARQ: SL5 from " + std::to_string(mq3m_.size() + 1) + " copies"); }
		else mq_keep(5, std::move(dd), {});
		return;
	}
	std::vector<std::complex<double>> d0, d1;
	const p3::LvSync sy = p3::sync_level(lv, x.data(), long(x.size()), fs, t0, search_ms);
	const p3::LvRx r = p3::finish_level(lv, x.data(), long(x.size()), fs, sy, 0, mq ? &d0 : nullptr, mq ? &d1 : nullptr);
	if (r.ok) { level = lv; status = r.status; data = r.data; t_pkt = r.t0; return; }
	if (!mq || d0.empty()) return;
	for (int m0 = 0; m0 < 2 && !mq3m_.empty(); m0++) {
		auto sum = m0 ? d1 : d0;
		for (size_t c = 0; c < mq3m_.size(); c++) {
			const bool odd = ((mq3m_.size() - c) & 1) ? !m0 : m0;   // the map alternates per transmission
			const auto &d = odd ? mq3m_[c].d1 : mq3m_[c].d0;
			for (size_t w = 0; w < sum.size() && w < d.size(); w++) sum[w] += d[w];
		}
		const p3::Lv23Rx rr = p3::decode_level_diffs(lv, sum);
		if (rr.ok && mq_cnt_ok(rr.status)) { level = lv; status = rr.status; data = rr.data; t_pkt = sy.t; dbg("memory ARQ: SL" + std::to_string(lv) + " from " + std::to_string(mq3m_.size() + 1) + " copies"); return; }
	}
	mq_keep(lv, std::move(d0), std::move(d1));
}

std::vector<std::complex<double>> P2Monitor::long_products(int level, const std::vector<float> &x, double t, bool odd) const
{
	const double fs = rx_.cfg_.fs;
	const p3::Layout &L = p3::layout_long(level);
	std::vector<std::complex<double>> d(size_t(L.nwords()), std::complex<double>(0, 0));
	if (!L.narrow) {
		const auto zf = p3::demod_flat(level, x.data(), long(x.size()), fs, t, 0, nullptr, -1, true);
		for (int p = 1; p < L.npulses(); p++)
			for (int j = 0; j < L.carriers(p); j++) {
				const int k = L.carrier(p, j, odd);
				d[size_t(L.word_index(p, j))] = zf[size_t(p)][size_t(k)] * std::conj(zf[size_t(p - 1)][size_t(k)]);
			}
	} else {
		const auto z = p3::demod_lv(level, x.data(), long(x.size()), fs, t, odd, 0, nullptr, -1, true);
		for (int p = 1; p < L.npulses(); p++)
			for (int j = 0; j < L.carriers(p); j++) {
				const int k = L.carrier(p, j, odd);
				int jp = -1;
				for (int q = 0; q < L.carriers(p - 1); q++) if (L.carrier(p - 1, q, odd) == k) jp = q;
				if (jp >= 0) d[size_t(L.word_index(p, j))] = z[size_t(p)][size_t(j)] * std::conj(z[size_t(p - 1)][size_t(jp)]);
			}
	}
	return d;
}

p3::LongRx P2Monitor::long_from_products(int level, const std::vector<std::complex<double>> &d)
{
	std::vector<double> st(d.size(), 0.0), wt(d.size(), 1.0);
	double m = 0; int n = 0;
	for (size_t w = 0; w < d.size(); w++) {
		const double v = -std::arg(d[w]) / (2 * M_PI) * 16; st[w] = v < 0 ? v + 16 : v;
		const double a = std::abs(d[w]); if (a > 0) { m += a; n++; }
	}
	if (n) for (size_t w = 0; w < d.size(); w++) wt[w] = std::abs(d[w]) / (m / n);
	return p3::long_decode(level, st.data(), n ? wt.data() : nullptr);
}

void P2Monitor::deliver(int who, const uint8_t *info, int n, int status)
{
	if (status >= 0 && (status & 3) == last_cnt_[who] && status >= 0) return;   // a repeat
	if (status >= 0) last_cnt_[who] = status & 3;
	const int type = status < 0 ? 0 : (status >> 2) & 7;
	std::string t = rle_[who].feed(text::decode(type, info, n, phase_ == P2DATA));
	std::string clean;
	for (char ch : t) if (ch != 0x1e) clean += ch;
	text_out(who, clean);
}

}  // namespace fmodem

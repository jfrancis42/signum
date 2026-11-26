// monitor.hpp: monitor mode (TODO B), first version. Listens to someone
// else's QSO and decodes both sides: the P1 call (any callee),
// the callee's answer, the caller's set-up packet (its call, P2/P3
// offer), the callee's accept, then the P2 data phase in both
// directions: each cycle the sender's packet and the receiver's control
// signal, a break-in swapping the roles, the QRT ending it. Text is decoded
// per direction (data type, run-length escapes) and each packet delivered
// once (its counter). P3 is followed by its bursts instead: each found
// by its energy and decoded at every level (short, long, SL1, break-ins),
// the sender told by its phase on the 1.25 s cycle. P1 links are followed
// with one passive P1Station per sender (README fix 112), and a QSO already
// running is joined from its bursts (fix 123).
//
// It drives a passive P2Station (never transmitting, never answering) as its
// receiver, so every decoder is the station's own.
#pragma once

#include <deque>
#include <string>
#include <vector>

#include "p2_station.hpp"

namespace fmodem {

class P2Monitor {
public:
	struct Line { double t; int who; std::string text; };   // who: 0 caller, 1 callee, -1 the monitor's own note

	explicit P2Monitor(const P2Config &c) : rx_(c), rx3_(c), p1rx_{P1Station(p1cfg(c)), P1Station(p1cfg(c))}
	{
		rx_.state_ = rx3_.state_ = P2Station::ST_IDLE; rx3_.p3_ = true;
		for (auto &p : p1rx_) { p.state_ = P1Station::ST_IDLE; p.cfg_.inline_status = false; }
	}

	void set_debug(bool d) { debug_ = d; }
	// One received sample (9600 Hz).
	void tick(int16_t x);
	std::deque<Line> take() { std::deque<Line> l; l.swap(out_); return l; }
	const std::string &call(int who) const { return who ? callee_ : caller_; }
	bool in_qso() const { return phase_ != SEARCH; }
	// End of the input (a file): text not yet ended by a CR.
	void finish() { flush(0); flush(1); }

private:
	enum Phase { SEARCH, CALL, SETUP, ACCEPT, P2DATA, P3DATA, P1DATA };
	enum Kind { K_CS, K_SETUP, K_ACCEPT, K_PACKET, K_REPLY, K_BKDATA, K_P1PKT, K_P1CS, K_P1BK, K_P3LAT, K_P3ACQ, K_P3B1 };
	struct Ev { int64_t at; Kind k; int64_t ref; int arg; };

	P2Station rx_, rx3_;
	P1Station p1rx_[2];
	static P1Config p1cfg(const P2Config &c) { P1Config q; q.fs = c.fs; q.tone_lo = c.tone_lo; q.tone_hi = c.tone_hi; return q; }
	bool debug_ = false;
	Phase phase_ = SEARCH;
	std::deque<Ev> ev_;
	std::deque<Line> out_;
	std::string caller_, callee_;
	int64_t call_first_ = 0, setup_first_ = 0, slot_ = 0;
	int setup_tries_ = 0, iss_ = 0, level_ = 2, misses_ = 0, last_cnt_[2] = {-1, -1};
	bool long_ = false;
	text::RleExpander rle_[2];
	std::string pending_[2];                           // text not yet ended by a CR

	int64_t S(double s) const { return rx_.samples(s); }
	double T(int64_t n) const { return double(n) / rx_.cfg_.fs; }
	void at(int64_t when, Kind k, int64_t ref, int arg = 0)
	{
		Ev e{when, k, ref, arg};
		auto it = ev_.begin();
		while (it != ev_.end() && it->at <= when) ++it;
		ev_.insert(it, e);
	}
	// Notes end the stations' partial lines first, so text comes before the event that followed it.
	void note(const std::string &s) { if (s.empty() || s[0] != '(') { flush(0); flush(1); } out_.push_back({T(rx_.n_), -1, s}); }
	void dbg(const std::string &s) { if (debug_) note("(" + s + ")"); }
	// All decoded text, every mode: one line buffer per station, emitted a
	// line at a time (at CR, at a note, at 200 characters) and always labelled
	// with its sender. (Packets were printed as they came before: text after a
	// CR inside a packet came out on an unlabelled line, 2026-10-20.)
	void text_out(int who, const std::string &t);
	void reset();
	static bool call_char(uint8_t c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '/' || c == '-'; }

	// ---- the P1 call: 0x55 + the callee, 8 bytes, 0F-padded, 100 Bd ----
	bool call_at(double t, bool inv, std::string &who) const;
	void search_call();
	// ---- events -------------------------------------------------------------
	void handle(const Ev &e);
	// The callee's answer to the call (first bit `ref`): CS1 (200 Bd) or CS4.
	void on_cs(int64_t ref);
	bool slow_ = false, setup_100_ = false, setup_noted_ = false;
	// The caller's set-up packet at 100 Bd (after CS4): header, 8 data bytes,
	// status, CRC over data and status; first bit near `exp` (+-12 ms).
	bool setup100_at(int64_t exp, int64_t &t_out, uint8_t *bytes);
	// The caller's set-up packet, first bit near `exp`.
	void on_setup(int64_t exp);
	void on_accept(int64_t t1);
	// ---- P1 data phase ------------------------------------------------
	// The sender's packet every 1.25 s (first bit p1_slot_), 100 or 200 Bd;
	// the receiver's CS after it: CS1/CS2 acknowledge (a repeat keeps the
	// counter), CS4 changes speed, CS3 heads a break-in packet: the receiver
	// sends from then on, its packets 1.25 s after its break-in. A packet with
	// status bit 7 ends the link. [spec: P1 description; our P1Station]
	int p1_iss_ = 0, p1_last_cnt_[2] = {-1, -1}, p1_fails_ = 0;
	int64_t p1_slot_ = 0;
	double p1_reply_ = 1.07;                          // the receiver's CS after the sender's first bit, s (measured from the CSs heard)
	void p1_text(int who, const std::string &t) { text_out(who, t); }
	void p1_deliver(int who, const P1Station::RxPacket &p);
	void flush(int w) { if (!pending_[w].empty()) { out_.push_back({T(rx_.n_), w, pending_[w]}); pending_[w].clear(); } }
	void p1_end(const std::string &why)
	{
		for (int w = 0; w < 2; w++) flush(w);
		note("P1 link " + caller_ + " - " + callee_ + " ended (" + why + ")");
		reset();
	}
	void on_p1_packet(int64_t slot);
	void on_p1_cs(int64_t slot);
	void on_p1_bk(int64_t when);
	// ---- P2 data phase ------------------------------------------------
	// The sender's packet in the slot at `slot` (waited: decided after a long
	// packet's end).
	void on_packet(int64_t slot, int mode);               // mode 0: first look, 1: a long packet (waited), 2: long or reply?
	int64_t pending_slot_ = 0, last_bk_ = -1000000;
	bool qrt_seen_ = false;
	int after_qrt_ = 0, last_cs_ = -1;
	void on_reply(int64_t exp);
	// A break-in from the receiver, started at `bk`: its data, then it sends.
	void on_bk_data(int64_t bk);
	// ---- P3 (by bursts) --------------------------------------------------
	// After the P3-accept every burst is found by its energy and decoded at every
	// level (p3replay's wide search); a decoded packet whose start keeps the
	// sender's phase on the 1.25 s cycle is the sender's, one at another phase
	// means the other station has taken over (the caller sends first).
	int64_t p3_anchor_ = 0, b_start_ = 0, b_last_ = 0;
	int p3_sender_ = 0, quiet_ = 0;
	double p3_phase_ = -1, nf_ = -1;
	bool b_on_ = false;
	int lvl_hint_ = 0, fails_ = 0;
	struct MqCopy { int level; std::vector<std::complex<double>> d0, d1; };
	std::vector<MqCopy> mq3m_;                         // failed copies of the current sender's packet (memory ARQ)
	void mq_keep(int lv, std::vector<std::complex<double>> d0, std::vector<std::complex<double>> d1)
	{
		if (!mq3m_.empty() && mq3m_.back().level != lv) mq3m_.clear();
		mq3m_.push_back({lv, std::move(d0), std::move(d1)});
		if (mq3m_.size() > 3) mq3m_.erase(mq3m_.begin());
	}
	bool mq_cnt_ok(int status) const { return last_cnt_[p3_sender_] < 0 || (status & 3) == ((last_cnt_[p3_sender_] + 1) & 3); }
	void p3_bursts();
	std::vector<float> window(int64_t a, int64_t b) const
	{
		std::vector<float> x(size_t(std::max<int64_t>(0, b - a)));
		for (int64_t t = a; t < b; t++) x[size_t(t - a)] = (t >= 0 && t < rx_.n_ && rx_.n_ - t < P2Station::kRing - 1) ? rx_.raw_[size_t(t % P2Station::kRing)] : 0.0f;
		return x;
	}
	// A burst may hold the receiver's control signal and then the sender's
	// packet (in the mix they can follow within 50 ms): the packet is tried
	// where the burst starts and where a packet ending with the burst starts.
	void p3_burst(int64_t s, int64_t e);
	bool p3_burst_at(int64_t s, int64_t e, bool long_burst);
	// ---- the sender's lattice: its next packet exactly 1.25 s on ----------------
	// Once a packet decodes, the sender's next one starts 1.25 s later: decoded
	// there with a narrow timing search, whatever the energy detector saw (at
	// noise 2500 it saw no burst at all; the stations decode on their cycle).
	// Memory ARQ runs here, over the failed copies on the lattice.
	bool lat_on_ = false;
	int64_t lat_next_ = 0;
	int lat_gen_ = 0, lat_miss_ = 0;
	void lat_start(int64_t next)
	{
		lat_on_ = true; lat_next_ = next; lat_miss_ = 0;
		at(lat_next_ + S(0.92), K_P3LAT, lat_next_, ++lat_gen_);
	}
	void on_p3_lattice(int64_t t, int gen);
	// Acquisition without the energy detector (noise 2500: no burst seen): each
	// cycle of the set-up's lattice, the caller's data phase (0.606 s) and the
	// callee's (0.885 s) are decoded; the first packet starts the lattice.
	int64_t last_dec_ = 0;
	bool retimed_ = false;
	int64_t b1_at_ = -1;                               // the callee's B1, when decoded by its timing
	// ---- joining a QSO already running (no call or set-up heard) -----------
	// In SEARCH, every packet-length burst (0.7-1.15 s short, 2.9-3.8 s long)
	// is tried as P3 (every level), P2 (levels 1-4) and P1
	// (100/200 Bd). The first decode enters that mode's data phase anchored on
	// the packet; its sender is "STN1", the other "STN2" (the calls are only
	// in the call and the set-up). From there the normal following runs.
	bool jb_on_ = false; int64_t jb_start_ = 0, jb_last_ = 0, join_after_ = 0; double jnf_ = -1;
	void join_bursts();
	void try_join(int64_t s, int64_t e, bool lng);
	// The callee's B1 (its set-up answer, in the break-in format) carries the
	// first characters of its greeting, 0.885 s into the caller's A1 cycle
	// (our station's set-up, the protocol description 4). Decoded by its time: the
	// set-up is near-continuous signal, which the burst detector's floor
	// follows up, so B1 never shows as a burst (2026-10-20: "PT" of the
	// greeting lost). Repeated a cycle later if the caller did not take it.
	void on_p3_b1(int64_t start, int tries);                             // p3_burst_at: inside the shifted retry
	int acq_left_ = 12;                                // acquisition cycles left (the cost: up to 4 phases x 3 levels a cycle)
	void on_p3_acquire(int64_t cyc0);
	// One level at timing t0 (+-search ms); memory ARQ over the stored copies when mq.
	void p3_decode_level(int lv, const std::vector<float> &x, double t0, double search_ms, bool mq, int &level, int &status, std::vector<uint8_t> &data, double &t_pkt);
	// p3replay's long-packet decode: per-word differential products, soft steps weighted by their size
	std::vector<std::complex<double>> long_products(int level, const std::vector<float> &x, double t, bool odd) const;
	static p3::LongRx long_from_products(int level, const std::vector<std::complex<double>> &d);
	void deliver(int who, const uint8_t *info, int n, int status);
};

}  // namespace fmodem

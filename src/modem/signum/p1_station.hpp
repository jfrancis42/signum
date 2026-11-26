// P1 (Level I) ARQ station, C++17.
//
// Protocol: the 1990 description by DL6MAA/DF4KV.
// On-air details the description leaves open were measured from the reference unit
// signals (emulated firmware V.4.1r, confirmed against the real unit):
//   * every burst starts with TXD (20 ms) of the mark tone of its polarity;
//   * polarity (shift) inverts with every transmission of a station;
//   * call packet: 55 + callee (8 bytes, 0F-padded) at 100 Bd, then 6 bytes
//     at 200 Bd: the callee + NUL written over "*SCSPTC*";
//   * slave answers (CS, or its BK packet) 1.003 s after the master's packet
//     first bit; master as receiver answers 1.095 s after the slave's packet;
//   * BK packet at 200 Bd: CS3 (100 Bd) then 144 data bits + status + CRC;
//   * QRT packet carries the other station's call as a 100 Bd bit pattern
//     (each bit doubled) followed by the header byte, status bit 7 set;
//   * first text from the master: level digit + lowercase own call + CR.
//
// The station is a pure function of its audio input: call tick() once per
// sample with the received sample; it returns the sample to transmit and
// keeps ptt() up to date. Host and ESP32 alike drive it from a codec.
#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "compress.hpp"
#include "crc16.hpp"
#include "fsk_discriminator.hpp"
#include "p1_huffman.hpp"

namespace fmodem {

struct P1Config {
	float fs = 9600.0f;
	float tone_lo = 1400.0f, tone_hi = 1600.0f;   // TONES 4 (P-III standard dial tones)
	float txd_s = 0.020f;                          // TX delay: lead-in before the first bit
	float amplitude = 8000.0f;
	std::string mycall = "N0CALL";
	int timeout_cycles = 30;                       // no valid reply this long -> disconnect
	bool inline_status = true;                     // "*** CONNECTED/DISCONNECTED" lines in the received text (off: a host reports them)
	float rt_latency_s = 0.0f;                     // the audio path's calibration (calibration.conf rt_latency): callee replies leave this much earlier
	int rx_mute_delay = 0;                         // fix 243: deaf while transmitting, moved this many samples later (P2Config::rx_mute_delay)
	float rx_shift_hz = 0.0f;                      // fix 184: the peer heard this much above our tones (receive AFC)
	float tx_shift_hz = 0.0f;                      // fix 184: what we send moved by this much (P2Station's fix 145 transmit AFC, kept through the hand-over)
	// Break-in as IRS (owner decision a, 2026-11-15). Default (the reference unit, PDuplex 0): we break in
	// only when the sender hands over (status bit 6), when our user asks (the break-in key, host %O/%I:
	// request_break_in) or for our QRT; text typed while receiving waits. pduplex (PDuplex 1): also
	// whenever text is waiting, but not within pdtimer_s of the other station taking the link from us
	// by a break-in (PDTimer, 2-30 s, default 12; the countdown starts at the other's CS3, 1 s ticks).
	bool pduplex = false;
	float pdtimer_s = 12.0f;
};

enum Cs : uint8_t { CS_NONE = 0, CS1 = 1, CS2 = 2, CS3 = 3, CS4 = 4 };
inline constexpr uint16_t kCsBits[5] = {0, 0x4D5, 0xAB2, 0x34B, 0xD2C};

class P2Monitor;

class P1Station {
	friend class P2Monitor;                        // monitor mode: passive decoders (find_packet, find_cs, deliver)
public:
	explicit P1Station(const P1Config &c);

	// Long path (manual 6.22.1, "C !CALL"): a 1.4 s cycle instead of 1.25 s. As in P2
	// (p2_station.hpp) [bb-emu, 2026-10-23]: the master keeps a 1.4 s grid; the slave's timing
	// relative to what it hears is unchanged; the call's first byte is inverted.
	bool long_path_ = false;
	double cyc_s() const { return long_path_ ? 1.4 : 1.25; }
	void set_long_path(bool on) { long_path_ = on; }
	bool long_path() const { return long_path_; }

	// ---- user interface -----------------------------------------------
	void connect(const std::string &call);
	void send_text(const std::string &t) { txq_ += escape(t); }
	void qrt() { want_qrt_ = true; }
	bool qrt_pending() const { return want_qrt_; }   // front panel: a disconnect (D) in progress
	// The user's turn keys. KEY_BK (BKchr, host %I): as receiver, break in at the next packet taken
	// (as the reference unit). KEY_CHO (CHOchr): as sender, hand the link over
	// once the text typed so far has gone (status bit 6; the reference unit's changeover marker
	// FE 01). KEY_BK | KEY_CHO (host %O; the one key when BKchr = CHOchr, the default
	// Ctrl-Y): whichever applies. Returns false when there is nothing to do.
	enum { KEY_BK = 1, KEY_CHO = 2 };
	bool changeover_key(int keys)
	{
		if ((keys & KEY_BK) && (state_ == ST_IRS || state_ == ST_BK_RX)) { bk_req_ = true; log("break-in requested"); return true; }
		if ((keys & KEY_CHO) && (state_ == ST_ISS || state_ == ST_BK_SENT)) { cho_req_ = true; log("changeover requested"); return true; }
		return false;
	}
	void set_pduplex(bool on, float timer_s) { cfg_.pduplex = on; cfg_.pdtimer_s = timer_s; }
	void set_debug(bool d) { debug_ = d; }
	void set_rt_latency(float s) { cfg_.rt_latency_s = s; }   // self-timing (the real-time audio loop)
	bool connected() const { return state_ != ST_IDLE && state_ != ST_LISTEN && state_ != ST_CALLING; }
	// Front panel: calling, sending, the link's speed (200 or 100 Bd), the other station.
	bool calling() const { return state_ == ST_CALLING; }
	bool sending() const { return state_ == ST_ISS || state_ == ST_BK_SENT; }
	// The front panel (measurement only): the meaning of the last control signal while we send, the
	// P2Station's FAR_* bits (1 OK, 2 repeat, 4 faster, 8 slower); 0: nothing heard, or older than 1 s.
	int far_end() const { return sending() && far_n_ >= 0 && n_ - far_n_ < samples(1.0) ? far_ : 0; }
	int link_baud() const { return fast_ ? 200 : 100; }
	// Host status (fix 166): a break-in in progress, ours (we were receiving) or the other's (we
	// were sending); whether the last packet received carried text; the speed of what we
	// receive while receiving, of what we send while sending.
	bool bk_ours() const { return state_ == ST_BK_SENT; }
	bool bk_theirs() const { return state_ == ST_BK_RX; }
	bool last_rx_text() const { return last_rx_text_; }
	int cur_baud() const { return (state_ == ST_IRS || state_ == ST_BK_RX ? rx_fast_ : fast_) ? 200 : 100; }
	const std::string &remote_call() const { return remote_; }
	bool idle() const { return state_ == ST_IDLE; }
	// ---- taking over a link the P2/P3 station set up (p2_station.hpp) ---
	// The P2 station answers and places every call; when the peer turns out to
	// be P1 only (or MYLevel 1), it creates a P1Station, replays its own
	// recent receive audio into it from begin_at() on, and hands the link over.
	// Stand-by afterwards is ST_IDLE: the P2 station listens for calls.
	// Fix 234: the audio layer saw our round trip move (as P2Station::note_audio_slip, fix 219):
	// for 30 s our receiver's look after a missed packet widens (rx_search_s).
	void note_audio_slip() { slip_hint_n_ = n_; }
	void begin_at(int64_t n) { n_ = n; state_ = ST_IDLE; esc_pending_ = false; si_ = text::SiFilter(); }
	// Callee: we answered the call whose first bit was at call_first with
	// `ack` (CS1 at 200 Bd, CS4 at 100 Bd); the caller's first packet follows
	// 1.25 s after it. next_inv: our next burst's polarity.
	void adopt_slave(const std::string &caller, int64_t call_first, Cs ack, bool next_inv);
	// Caller: our call (first bit call_first: our transmit grid) was answered,
	// our first data packet (first bit pkt_first, header and counter as sent)
	// acknowledged with `acked`; send the next one, from txq.
	void adopt_master(const std::string &callee, int64_t call_first, int64_t pkt_first, bool fast, uint8_t header, int counter, Cs acked, bool next_inv, const std::string &txq);
	// Fix 207: the P2 station that set the link up heard the callee's replies to our call and set-up
	// packet clearly (>= 0.95) at `off` samples after our first bit; fix 183's reply track starts
	// from there instead of from nothing (`ref`: the first bit of our last burst).
	void seed_reply_track(double off, int64_t ref)
	{
		if (std::getenv("P1_NO_TRACK_SEED")) return;
		cs_trk_off_ = off; cs_trk_n_ = 3; cs_trk_away_ = 0; cs_trk_last_ = ref;
		cs_ref_off_ = off; cs_ref_ok_ = true;
		log("reply track from the set-up: +" + std::to_string(off / cfg_.fs) + " s");
	}
	size_t tx_unsent() const { return txq_.size() - size_t(std::max(0, pending_used_)); }
	size_t tx_unacked() const { return size_t(std::max(0, pending_used_)); }
	const std::string &end_reason() const { return why_; }   // why the last link ended
	// Transparency [bb-ext: the reference firmware in host mode, P1 link, all 256
	// byte values, 2026-10-20; the scheme is the 1990 description's supervisor
	// character]: FS (1C) + (c + 60) stands for c in {0E 0F 10 14-17 1C 1E 1F};
	// every other byte goes as it is. RS (1E) is the idle symbol, so without it
	// a binary 1E was lost (Pat's B2F over P1 failed when a block held one).
	static bool escaped_ctl(uint8_t c) { return c == 0x0E || c == 0x0F || c == 0x10 || (c >= 0x14 && c <= 0x17) || c == 0x1C || c == 0x1E || c == 0x1F; }
	static std::string escape(const std::string &t)
	{
		if (std::getenv("P1_NO_ESCAPE")) return t;     // test: the old, lossy way
		std::string o;
		for (char ch : t) { if (escaped_ctl(uint8_t(ch))) { o += char(0x1C); o += char(uint8_t(ch) + 0x60); } else o += ch; }
		return o;
	}
	bool tx_pending() const { for (const auto &b : bursts_) if (b.end + 2 >= n_) return true; return false; }   // a burst queued or on the air (the last CS after a QRT)
	bool ptt() const { return ptt_; }
	std::string take_rx() { std::string s; s.swap(rx_); return s; }
	std::string take_log() { std::string s; s.swap(log_); return s; }

	// ---- one audio sample in, one out ----------------------------------
	int16_t tick(int16_t rx_sample);

private:
	// ---- types --------------------------------------------------------
	enum State { ST_IDLE, ST_LISTEN, ST_CALLING, ST_ISS, ST_IRS, ST_BK_SENT, ST_BK_RX };
	enum EvType { EV_CS_WINDOW, EV_PACKET_DUE, EV_BK_DONE };
	struct Event { int64_t at; EvType type; int64_t ref; };
	struct Seg { int baud; std::vector<uint8_t> bits; };
	struct Burst { int64_t first_bit; bool inv; std::vector<Seg> segs; int64_t onset, end; };

	static constexpr int64_t kRing = 1 << 16;      // ~6.8 s of soft samples at 9600 Hz

	// ---- timing helpers -------------------------------------------------
	int64_t samples(double s) const { return int64_t(std::llround(s * cfg_.fs)); }
	double spb(int baud) const { return cfg_.fs / baud; }

	void log(const std::string &s) { log_ += "[pt1] " + s + "\n"; }

	// ---- receive helpers --------------------------------------------------
	// Integrated soft value of one bit starting at absolute sample `a` (on-air
	// time). The discriminator output lags the air by its FIR group delay, so
	// look that much later: all timestamps we derive are then true air times
	// and our replies leave exactly when the protocol says.
	double bit_soft(double a, int baud) const;
	// Hard bits (inverted if `inv`) starting at `first` (first bit start).
	void bits_at(double first, int baud, int nbits, bool inv, uint8_t *out) const
	{
		for (int k = 0; k < nbits; k++) {
			const int b = bit_soft(first + k * spb(baud), baud) > 0 ? 1 : 0;
			out[k] = uint8_t(b ^ (inv ? 1 : 0));
		}
	}
	static void pack(const uint8_t *bits, int n, uint8_t *bytes)
	{
		std::memset(bytes, 0, size_t((n + 7) / 8));
		for (int i = 0; i < n; i++)
			if (bits[i]) bytes[i / 8] |= uint8_t(1u << (i % 8));
	}

	// Best CS (soft correlation) with first bit in [a, b]; returns score.
	double find_cs(int64_t a, int64_t b, Cs &cs, int64_t &at, bool &inv) const;
	// Fix 228: the second look (see on_cs_window): a control signal at the expected time and with the
	// expected polarity, without the weak-bit rule. Codes of one polarity are 8 or more bits apart (the
	// polarity-free search above has CS1 3 bits from an inverted CS2), so the strong bits alone decide.
	double find_cs_expected(int64_t a, int64_t b, bool want_inv, Cs &cs, int64_t &at) const;
	static bool cs_all_bits() { static const bool o = std::getenv("P1_CS_ALL_BITS") != nullptr; return o; }
	// Power at the two FSK tones against two tones outside the signal, in dB, over n samples
	// from t0 (Goertzel on the received audio); what has arrived, at least 3/4 of it (as
	// P2Station::p1_tone_snr_db).
	double tone_snr_db(int64_t t0, int n) const;

	struct RxPacket { int baud = 0; bool inv = false; uint8_t header = 0, status = 0; uint8_t data[20]{}; int data_bits = 0; int64_t first = 0; bool bk = false; };

	// Fix 249: as IRS, a packet's hard bits taken only at its best timing (the most bit energy, as
	// memory ARQ's), not at every second sample of the +-10 ms look. Each timing tried is another
	// error pattern offered to the 16-bit CRC: near the packet's true timing they are different
	// patterns of the same few wrong bits, and now and then one of them passes. The P1
	// p1-rx-stepdown (noise 3,500 on our receiver), 9 runs (CHAN_SEED 0-8, against the reference firmware): 4 delivered
	// a packet that passed the CRC with 4-7 wrong bits (its counter the expected one, its text
	// garbled: "cho n o \rd bra" for "cho golf bra"), ending the exact count at 441 / 107 / 312 / 857;
	// the reference unit pair's runs never did. The CRC is now tried at
	// the best timing and up to kFocusHalf samples either side, and the same a whole bit or more away
	// within the look (the bit energy does not tell which bit starts the packet; a whole bit off, the
	// bits are shifted and the header check refuses them, so these add no error patterns of the
	// packet): every second sample, both polarities, both speeds. Measured on p1-rx-stepdown, CHAN_SEED 0-17, with
	// memory ARQ's timing below: +-6 no wrong packet in 18 runs (832-990 characters); +-12 one in 18;
	// the old look 1 in 14 even with the better memory ARQ. A wrong packet costs the link's text from
	// there on; +-6 costs a few per cent of the packets a wider look reads. P1_RX_FOCUS_OLD=1: every
	// timing of the look and memory ARQ's +-6, as before; P1_RX_FOCUS_HALF=N: other half-width.
	static constexpr int kFocusHalf = 6, kMarqHalf = 30;
	static int focus_half() { static const int v = std::getenv("P1_RX_FOCUS_HALF") ? std::atoi(std::getenv("P1_RX_FOCUS_HALF")) : kFocusHalf; return v; }
	static bool focus_old() { static const bool v = std::getenv("P1_RX_FOCUS_OLD") != nullptr; return v; }
	int64_t best_timing(int64_t a, int64_t b, int baud) const;
	// fix 249: t is a timing tried: within focus_half() of the best timing's phase (whole bits either way:
	// the bit energy fixes the timing within a bit, not which bit), every second sample
	bool focus_take(int64_t t, int64_t ft, int baud) const
	{
		const double w = spb(baud), d = double(t - ft), r = d - w * std::round(d / w);
		const int64_t ri = int64_t(std::llround(r));
		return std::llabs(ri) <= focus_half() && ri % 2 == 0;
	}
	// Look for a CRC-valid packet with first bit in [a, b] (normal or BK). focus: fix 249.
	bool find_packet(int64_t a, int64_t b, RxPacket &p, bool allow_bk, bool focus = false) const;

	// ---- memory ARQ [spec: ARRL protocol description §1: "store the relative
	// strength of each received bit. Copies of corrupted frames stored this way
	// are correlated with frames received later"] ----------------------------
	// A packet that failed: its bits' soft values at its best timing (within
	// +-6 samples of `exp`, on the packet's bit energy) are kept; each later copy is added. The copies' signs are aligned
	// by their correlation (the shift inverts with every transmission [bb],
	// and a corrupted copy is still mostly right). The sum is decoded with the
	// same header and CRC checks as a single copy. Cleared by any good packet.
	std::vector<std::vector<double>> marq_;
	int marq_baud_ = 0;
	void marq_clear() { marq_.clear(); marq_baud_ = 0; }
	bool marq_decode(int64_t exp, int baud, RxPacket &p);

	// ---- slave: listen for a call to us -----------------------------------
	void acquire_call();

	// ---- transmit side ----------------------------------------------------
	// The 200 Bd part of a call: the callee + NUL written over "*SCSPTC*".
	static std::vector<uint8_t> call200(const std::string &call)
	{
		std::vector<uint8_t> v{'*', 'S', 'C', 'S', 'P', 'T', 'C', '*'};
		for (size_t i = 0; i < call.size() && i < 8; i++) v[i] = uint8_t(call[i]);
		if (call.size() < 8) v[call.size()] = 0;
		v.resize(6);
		return v;
	}
	static void push_bits(std::vector<uint8_t> &v, const uint8_t *bytes, int nbits)
	{
		for (int i = 0; i < nbits; i++) v.push_back(uint8_t(bytes[i / 8] >> (i % 8) & 1));
	}
	static void push_value(std::vector<uint8_t> &v, uint32_t x, int nbits)
	{
		for (int i = 0; i < nbits; i++) v.push_back(uint8_t(x >> i & 1));
	}

	void queue_burst(int64_t first_bit, std::vector<Seg> segs, bool on_grid = false);

	void schedule_call();

	void send_cs(Cs c, int64_t first_bit)
	{
		std::vector<Seg> segs(1);
		segs[0].baud = 100;
		push_value(segs[0].bits, kCsBits[c], 12);
		queue_burst(first_bit, std::move(segs));
	}

	// Fill a data field: Huffman if it carries more text than ASCII-8.
	// Returns the number of characters consumed; sets the mode (0/1).
	int fill_data(const std::string &q, uint8_t *data, int dbits, int &mode) const;

	// Build the next data packet (new information) into pending_.
	void build_packet(bool new_info);

	// Resend the information of the rejected packet at the current speed:
	// same counter, payload re-packed for the new capacity.
	void rebuild_same()
	{
		const int dbits = fast_ ? 160 : 64;
		int mode = 0;
		if (!pending_qrt_) {
			pending_used_ = fill_data(txq_, pending_data_, dbits, mode);
			pending_status_ = uint8_t(counter_ | mode << 2);
		}
		build_packet(false);
	}

	// BK packet: CS3 head, then data (counter 0).
	// repeat: the same break-in again (no acknowledgement heard). It carries
	// exactly the characters of the first: refilled from the queue, a repeat of
	// an empty break-in took the host's newly queued text; the receiver had
	// taken the first one, answered the repeat as a repeat, and that text was
	// dropped as delivered (Pat's SID over P1, 6 m, 2026-10-20).
	// P1_BK_REFILL: the old refill (test).
	void send_bk(int64_t first_bit, bool repeat = false);

	// The slave sending follows the master's grid. A reference unit master keeps a fixed grid also as
	// receiver, and its slave times its data packets to the master's control signals
	// (emulated V4.1r pairs with a clock offset: the slave's packets within +-4 ppm of the master's
	// grid at offsets up to 65 ppm) [bb-emu, the capture generator]. Ours stayed
	// on the grid of its break-in: at the reference unit's own 62 ppm our packets walked out of its
	// window and the link died (fix 163). Each clear
	// control signal (score >= 0.95) gives the master's position in our original lattice (the
	// reading plus our shifts so far); the base is the mean of the turn's first 4; the held value
	// the mean of the middle 4 of the last 6 (the reading alternates by ~2 ms with the shift's
	// polarity under fading: the clean tree's P1-20, ported); our packets move toward held - base
	// by at most 0.5 ms a cycle once 0.5 ms or more away. Only as slave: a master that followed a
	// slave which follows it would run away. P1_NO_SLAVE_FOLLOW: off (old).
	//
	// Fix 192 (OFF by default; P1_MASTER_FOLLOW=1: on): the master follows too (the clean tree's
	// P1_TX_FOLLOW, its PROVENANCE P1-20/21, ported): as sender its next packet moves with the
	// slave's control signals (fl_, as the slave's), as receiver the grid its replies sit on
	// (grid0_) moves with each directly decoded packet's first bit in our grid slot (fr_). The
	// reference unit master does neither [bb-emu]; its slave corrects within 30 ppm only (manual 6.71), so
	// a slave clock off by more loses the link with a reference unit master or with ours: our clock 65 /
	// 130 ppm slow without audio_ppm, the reference unit as slave: 0-517 of 1,500 without, 1,500
	// with it. But against a slave that follows us it runs away, as noted above: the slave keeps
	// the reply time where we put it, so a base 0.5 ms off the steady value is never reached and
	// we step 0.5 ms every few cycles in one direction: -11.5 ms (we send) and -14 ms (we receive)
	// in ~200 s at no clock offset at all, emulator [bb-emu]; the clean side's bench -21 ms in 6
	// minutes against the real reference unit [bb-clean]. The sound card's error is corrected station-wide
	// instead (fix 165, audio_ppm); this stays for a peer whose own clock is off by more than its
	// follow range.
	struct Follow { double base_sum = 0, shift = 0; int base_n = 0; std::deque<double> hist; };
	Follow fl_, fr_;
	void follow_reset() { fl_ = Follow(); jc_have_ = false; }
	static bool master_follow_on() { static const bool on = std::getenv("P1_MASTER_FOLLOW") && std::atoi(std::getenv("P1_MASTER_FOLLOW")) != 0; return on; }
	int64_t follow_step(int64_t reading, Follow &f, const char *what);
	// The master transmits on its own fixed grid (first bits at grid0_ +
	// k*1.25 s) whatever the propagation delay; only the slave aligns to what
	// it receives. As receiver the master answers 0.845 s into the grid slot
	// nearest to the received packet's first bit + 0.25 s (reference timing).
	int64_t master_reply_time(int64_t rx_first) const;
	int64_t reply_time(int64_t rx_first, bool after_bk_packet) const;
	// Fix 211: our control signal to the master's break-in (we sent, it broke in; its CS3 first bit at
	// rx_first) 1.003 s after it, as to any of its packets; was 1.042 s. The reference unit did not
	// take the CS1 at 1.042 (nor at 1.020) and sent its break-in again: every turn it took cost a
	// cycle, on the reference unit and on the bench (p1-701/702: 2.5 s from its break-in to its next
	// packet) [bb-emu, P1_DIAG run c0-c2]. P1_BK_REPLY_S=S: other.
	static double slave_bk_reply_s() { static const double v = std::getenv("P1_BK_REPLY_S") ? std::atof(std::getenv("P1_BK_REPLY_S")) : 1.003; return v; }
	// Fix 195: our slave replies (control signals and break-ins) leave 2 ms earlier (the clean
	// tree's P1_SLAVE_ADVANCE_MS, its PROVENANCE P1-26/P1-27/E91/E92, ported). On the real reference unit as
	// master (bench, audio_ppm set) it removed the repeats of the reference unit's first packet when it
	// called and we sent: 0 of 8 runs with repeats at 2 or 4 ms against 5 of 12 at 0 ms, and break-ins
	// taken a copy or two sooner [bb-clean]. P1_SLAVE_ADVANCE_MS=A: A ms (0 = the old timing).
	static double slave_advance_s() { static const double a = (std::getenv("P1_SLAVE_ADVANCE_MS") ? std::atof(std::getenv("P1_SLAVE_ADVANCE_MS")) : 2.0) * 1e-3; return a; }

	// PDuplex 1 (or P1_BK_AUTO_OLD=1, the way before decision a: at once, no PDTimer): text waiting is
	// a reason to break in by itself.
	static bool bk_auto_old() { static const bool o = std::getenv("P1_BK_AUTO_OLD") != nullptr; return o; }
	static bool over_old() { static const bool o = bk_auto_old() || std::getenv("OVER_OLD") != nullptr; return o; }
	bool auto_break_in() const
	{
		if (txq_.empty()) return false;
		if (bk_auto_old()) return true;
		return cfg_.pduplex && n_ >= bk_hold_until_;
	}
	// IRS reply to the packet whose first bit we received at `rx_first`:
	// our CS (or a BK packet carrying our text, or the BK that precedes our
	// QRT) in our slot, then expect the next packet 1.25 s later.
	void reply_as_irs(int64_t rx_first, bool after_bk_packet);

	// ---- event handling ---------------------------------------------------
	void handle(const Event &e)
	{
		switch (e.type) {
		case EV_CS_WINDOW: on_cs_window(e.ref); break;
		case EV_PACKET_DUE: on_packet_due(e.ref); break;
		case EV_BK_DONE: on_bk_done(e.ref); break;
		}
	}

	// We transmitted (call, data, QRT or BK) with first bit at `ref`: what did
	// the other station answer?
	void on_cs_window(int64_t ref);

	// The IRS broke in; its BK packet (CS3 first bit at `at`) is complete.
	void on_bk_done(int64_t at);

	// We are IRS and a packet was due with first bit near `exp`.
	void on_packet_due(int64_t exp);

	// Fix 234 (bench p1-906): our capture lost ~38 ms in 10 s (the Pi's USB codec, fix 219: "capture
	// -1606 ppm", then "-5964 ppm"; the host held the round trip, 36.7 ms of silence added). Every
	// packet after it arrived that much earlier on our clock than the grid we looked on (+-10 ms
	// around one cycle after the last packet read): from the first miss on we never read another,
	// our control signals went on that stale grid (the reference unit repeated until its timeout), and the
	// link died ("rx nothing" x71). No break-in was involved: the packet before it was the first
	// one missed. Now, while the audio layer reports our round trip moving (note_audio_slip, within
	// 30 s), the look after a miss widens by 10 ms per miss to at most +-60 ms (our own control
	// signal ends ~1.12 s into the cycle, the next packet starts at 1.25 s); a packet read there
	// moves the grid with it (reply_as_irs: the next look and our reply follow its first bit).
	// Not otherwise: a wider look in noise alone is more chances of a random CRC pass.
	// P1_WIDEN_OFF=1: never (as before); P1_WIDEN_ALWAYS=1: test, without the audio layer's word.
	static constexpr double kSlipHintS = 30.0, kRxSearchMaxS = 0.060;
	int64_t slip_hint_n_ = -1, bk_rx_off_ = 0, jc_ref_ = 0;
	bool jc_have_ = false; double jc_off_ = 0;   // fix 234: a jump candidate (one reading), waiting for the next
	bool slip_hint_on() const
	{
		static const bool off = std::getenv("P1_WIDEN_OFF") != nullptr, always = std::getenv("P1_WIDEN_ALWAYS") != nullptr;
		return !off && (always || (slip_hint_n_ >= 0 && n_ - slip_hint_n_ <= samples(kSlipHintS)));
	}
	double rx_search_s() const
	{
		if (misses_ < 1 || !slip_hint_on()) return 0.010;
		return std::min(kRxSearchMaxS, 0.010 + 0.010 * misses_);
	}

	void deliver(const RxPacket &p);

	void disconnect(const std::string &why);

	static std::string lower(std::string s) { for (auto &c : s) c = char(std::tolower(c)); return s; }

	// ---- modulator ----------------------------------------------------------
	int16_t modulate();

	// ---- state ----------------------------------------------------------------
	P1Config cfg_;
	FskDiscriminator<> disc_;
	std::vector<float> soft_, raw_;
	std::vector<double> pre_;
	// Bits from the two tones' energies over the bit (non-coherent orthogonal
	// FSK: 200 Hz apart is 1/T at 200 Bd) instead of the integrated FM
	// discriminator, which has a threshold: white noise, 50 % of packets at
	// -1.6 dB SNR (was -0.6) at 200 Bd, 89 % at -4.3 dB (was 15 %) at 100 Bd
	// (a bench tool, 2026-10-20). P1_DISC: the discriminator.
	bool tone_det_ = std::getenv("P1_DISC") == nullptr;
	std::vector<std::complex<double>> chi_, clo_;
	int64_t n_ = 0;
	int far_ = 0;                                  // the front panel: the last control signal's meaning (far_end)
	int64_t far_n_ = -1;
	void far_set(int f) { far_ = f; far_n_ = n_; }
	std::vector<uint8_t> ptt_hist_;                // fix 243: our keying, per sample (only with rx_mute_delay)
	double gd_ = 0;
	int64_t grid0_ = 0;
	State state_ = ST_LISTEN;
	bool master_ = false, fast_ = true, ptt_ = false, tx_inv_ = false;
	std::string remote_, txq_, rx_, log_, why_, ident_buf_;
	bool ident_pending_ = false;                   // callee: the caller's "<level><call>\r" not yet read
	bool esc_pending_ = false;                     // received an FS: the next byte says what it stands for (P1_OLD_ESCAPE)
	text::SiFilter si_;                            // the SI layer: escapes, position reports, unknown SIs swallowed
	// Receiver speed changes: failures at 200 Bd before the CS4 that asks for 100 Bd (MAXDown), good
	// 100 Bd packets before the CS4 that asks for 200 Bd (MAXUp), cycles without a 200 Bd packet after
	// that CS4 before the speed-up is rejected (MAXTry). The reference V4.1r's own values
	// ("SPEED-DOWN-PARAMETER: 5", "SPEED-UP-PARAMETER: 4", "SPEED-UP-TRY-PARAMETER: 2" at its console,
	// and its receiver stepped down at the 5th bad packet and up at the 4th good one [bb-emu], fix 160).
	// P1_RX_DOWN / P1_RX_UP: other counts (test; the old 2 / 6).
	static int env_int(const char *k, int d) { const char *e = std::getenv(k); return e && *e ? std::max(1, std::atoi(e)) : d; }
	const int kRxDownAfter = env_int("P1_RX_DOWN", 5), kRxUpAfter = env_int("P1_RX_UP", 4);
	static constexpr int kRxMaxTry = 2;
	int rx_bad_ = 0, rx_good100_ = 0, rx_up_after_ = kRxUpAfter;
	bool rx_fast_ = true, rx_up_try_ = false, rx_up_wait_ = false, up_wait_ = false;
	int rx_up_fail_ = 0;
	static bool up_old() { static const bool o = std::getenv("P1_UP_OLD") != nullptr; return o; }
	std::deque<Event> events_;
	std::vector<Burst> bursts_;
	double phase_ = 0;
	int64_t next_first_bit_ = 0, my_first_ = 0, last_rx_first_ = 0;
	Cs ack_cs_ = CS1, last_rx_cs_ = CS_NONE;
	int cs_trk_n_ = 0, cs_trk_away_ = 0; double cs_trk_off_ = 0; int64_t cs_trk_last_ = -1;   // fix 183: the reply's timing
	double cs_ref_off_ = 0; bool cs_ref_ok_ = false;                    // fix 228: the last established reply track
	bool pol_ok_ = false, pol_inv_ = false; int64_t pol_ref_ = 0;      // fix 228: polarity of the last clear control signal, its window
	int counter_ = 0, last_rx_counter_ = -1, misses_ = 0, pending_used_ = 0;
	uint8_t header_ = 0x55, pending_status_ = 0, pending_data_[20]{};
	bool want_qrt_ = false, pending_qrt_ = false, after_bk_ = false, bk_sent_ = false;
	bool rx_ok_since_connect_ = false, debug_ = false, last_was_cs4_ = false;
	bool sent_text_ = false, must_break_in_ = false, last_rx_text_ = false;
	bool bk_req_ = false, cho_req_ = false;        // decision a: the user's break-in / changeover key (changeover_key)
	bool bk_asked_ = false;                        // our break-in in progress was asked for (key or the sender's changeover)
	int64_t bk_hold_until_ = 0;                    // PDuplex: no break-in on waiting text before this (PDTimer)
	int rej_total_ = 0;                            // fix 211: cycles in that state since the last acknowledgement
	int rej_cs4_ = 0;                              // fix 211: CS4s in a row to the 100 Bd copy of a REJECTed break-in (0: not in that state)
};

}  // namespace fmodem

// P2 ARQ station, C++17.
//
// The timing and behaviour here were measured against reference firmware V.4.1r
// (emulated, confirmed against the real unit); see the protocol description section 3.
//
//   Link set-up runs in P1 FSK (200 Bd): call -> CS1; the caller then
//   sends a P1 data packet "1<mycall>\r" with status bit 4 (0x11) = "P2?";
//   a P2 station answers with the 12-bit code 0x6A9 instead of a CS, and
//   the caller's next transmission (1.286 s after its P1 packet) is PSK.
//
//   PSK link: two carriers (tone pair), 10 ms pulses; data packets are
//   short (82 pulses, 1.25 s cycle) or long (330 pulses, 3.75 s cycle),
//   coded per p2_codec.hpp at speed level 1-4 (the header tells which).
//   Control signals: 22 pulses of DBPSK, the same bits on both carriers.
//     P / Q   acknowledgements, alternating (P, X, P, X ...)
//     R       = acknowledgement in the X position + toggle long packets
//     S       = acknowledgement in the X position + speed up one level
//     T       = speed down (sent after repeated failures)
//   A break-in (the receiver takes the link) is a packet whose header is a
//   24-pulse DBPSK pattern followed by 56 data pulses (7 per row).
//   Status byte: counter (bits 0-1), 0x20 more data follows, 0x40 over.
//
//   Timing (burst starts): the callee answers 0.879 s after the start of the
//   burst it answers, the caller 0.971 s; a station sending data sends its
//   next packet 1.25 s (long: 3.75 s) after its previous one.
//
// Same interface as P1Station: tick() once per sample.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <pthread.h>
#include <sched.h>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

#include "crc16.hpp"
#include "fsk_discriminator.hpp"
#include "compress.hpp"
#include "p2_codec.hpp"
#include "p3_modem.hpp"
#include "p3_rx.hpp"
#include "freq_search.hpp"
#include "p3_long.hpp"
#include "p3_unproto_rx.hpp"
#include "p3_papr.hpp"
#include "p1_station.hpp"
#include "features.hpp"
#include "busy_detector.hpp"
#include "rx_meter.hpp"

namespace fmodem {

// Fixed decode threads, started once. Creating a thread per decode costs
// the audio loop dearly under mlockall(MCL_FUTURE) (each new 8 MB stack is
// locked and faulted in while the process's memory map is held). Priority:
// par::set_worker_sched (FIFO 20 below the audio loop's 80). Tasks are
// taken in submission order, so a task may wait on one submitted before it
// (never after: that could deadlock). With 0 threads (small targets, see
// SIGNUM_DECODE_THREADS) a task runs at once, in the caller.
#ifndef SIGNUM_DECODE_THREADS
#define SIGNUM_DECODE_THREADS 4
#endif
class DecodePool {
public:
	explicit DecodePool(int n)
	{
		par::pool();   // the compute pool's threads too, now: not on the first decode while audio runs
		const bool bg = par::background_thread();   // made by the listen monitor: its workers at normal priority
		for (int i = 0; i < n; i++)
			th_.emplace_back([this, bg] {
				par::set_worker_sched(bg);
				for (;;) {
					std::function<void()> f;
					{
						std::unique_lock<std::mutex> lk(m_);
						cv_.wait(lk, [this] { return stop_ || !q_.empty(); });
						if (q_.empty()) return;
						f = std::move(q_.front()); q_.pop_front();
					}
					f();
				}
			});
	}
	~DecodePool()
	{
		{ std::lock_guard<std::mutex> lk(m_); stop_ = true; }
		cv_.notify_all();
		for (auto &t : th_) t.join();
	}
	int size() const { return int(th_.size()); }
	// urgent: to the front of the queue (our transmit audio, due within
	// ~50 ms: behind two queued decodes it was late, 2026-10-18). Safe with
	// the ordering rule: no task ever waits on an urgent one.
	template <class F> auto submit(F f, bool urgent = false) -> std::future<decltype(f())>
	{
		auto t = std::make_shared<std::packaged_task<decltype(f())()>>(std::move(f));
		auto fut = t->get_future();
		if (th_.empty()) { (*t)(); return fut; }
		{
			std::lock_guard<std::mutex> lk(m_);
			if (urgent) q_.emplace_front([t] { (*t)(); }); else q_.emplace_back([t] { (*t)(); });
		}
		cv_.notify_one();
		return fut;
	}
private:
	std::mutex m_;
	std::condition_variable cv_;
	std::deque<std::function<void()>> q_;
	std::vector<std::thread> th_;
	bool stop_ = false;
};

// Shift audio up by df Hz (single sideband: a centred Hilbert FIR, 95 taps; the burst is
// whole, so the filter costs no delay); `start` keeps the phase on the absolute sample clock.
// ssb_shift_range: output samples [i0, i1) of a burst of n samples into y; it reads a up to
// i1 + kSsbReach - 1 (a burst made in pieces is shifted as its pieces come, the same numbers).
constexpr long kSsbReach = 47;
inline void ssb_shift_range(const float *a, long n, float *y, long i0, long i1, double df, int64_t start, double fs)
{
	static const std::vector<double> h = [] {
		std::vector<double> t(95, 0.0);
		for (int k = 1; k <= 47; k += 2) {
			const double w = 0.54 + 0.46 * std::cos(M_PI * k / 48.0);
			t[size_t(47 + k)] = 2.0 / (M_PI * k) * w; t[size_t(47 - k)] = -t[size_t(47 + k)];
		}
		return t;
	}();
	const double dw = 2 * M_PI * df / fs;
	for (long i = i0; i < i1 && i < n; i++) {
		double q = 0;
		for (int k = 1; k <= 47; k += 2) {
			const double xm = i - k >= 0 ? a[size_t(i - k)] : 0.0, xp = i + k < n ? a[size_t(i + k)] : 0.0;
			q += h[size_t(47 + k)] * (xm - xp);
		}
		static const double ph0 = std::getenv("P2_TX_SHIFT_PHASE") ? std::atof(std::getenv("P2_TX_SHIFT_PHASE")) : 0.0;   // test
		const double ph = dw * double(start + i) + ph0;
		y[size_t(i)] = float(a[size_t(i)] * std::cos(ph) - q * std::sin(ph));
	}
}
inline void ssb_shift(std::vector<float> &a, double df, int64_t start, double fs)
{
	const long n = long(a.size());
	std::vector<float> y(a.size());
	ssb_shift_range(a.data(), n, y.data(), 0, n, df, start, fs);
	a.swap(y);
}

struct P2Config {
	float fs = 9600.0f;
	float tone_lo = 1400.0f, tone_hi = 1600.0f;
	float txd_s = 0.020f;                          // FSK lead-in (P1 phase)
	float amplitude = 8000.0f;
	std::string mycall = "N0CALL";
	int timeout_cycles = 70;                       // the firmware's MAXErr default ("TIMEOUT-PARAMETER: 70")
	bool realtime = false;                         // live audio: never block the audio loop on a decode
	// Highest level we send at and ask for (S) as receiver. 4 since
	// 2026-10-21: the real reference unit acknowledged our level-4 packets and
	// break-ins (the protocol description 3).
	int max_level = 4;
	bool request_speedup = true;                   // as receiver, ask for higher levels (S)
	// P3 receiver: while the computer cannot keep up (late or nearly late
	// decodes, CPU throttling), cap the speed level asked for (the design notes 3,
	// the operating notes "Decoding and the back-off")
	bool backoff = true;
	// The audio runs at its real rate (the real-time audio loop): only then is a
	// decode margin (wall time against the sample clock) meaningful. Not in
	// lockstep runs (the lockstep link with --realtime against the reference unit), which go as
	// fast as the CPU allows: no margins, no back-off there.
	bool paced = false;
	bool request_long = true;                      // as P2 receiver, ask for long packets (R)
	bool p3_request_long = true;                   // as P3 receiver, ask for long packets (R; fix 217: on, the firmware's rule)
	bool tx_afc = false;                           // P2: shift our transmissions by the offset we hear the peer at (experimental, off: see README fix 24)
	// Our reply instant relative to the nominal 0.879/0.971 s: the reference unit samples a
	// break-in on a fixed raster; level-4 break-ins are accepted first time for
	// +2..+5 ms (emulated firmware, 2026-10-13), so +3.5 ms.
	// Callee replies (P2): the code default is for links without an audio path (the reference unit);
	// over radios the calibration or self-timing sets it. The reference firmware takes our
	// replies with no level loss from -4 to +4.5 ms (2026-10-22, clean, firmware sending at
	// level 4); 3.5 ms (the 2026-10 value) sat at the edge and lost the step-down test (fix 136).
	float reply_adjust_s = 0.0f;
	float master_adjust_s = 0.0f;                  // caller replies (on its grid)
	// Caller's break-in only (fixes 130, 133): at level 4 the real reference unit takes it first time
	// from about -2 to +1 ms, the reference firmware from -3 to 0 ms; -1 ms is in both.
	float master_bk_adjust_s = -0.001f;
	// Callee break-in relative to our control-signal reply instant: the
	// emulated firmware accepted a P2 break-in from 4.2 ms early to 0 ms
	// (+0.1 ms late was refused, Q), so the centre of that window. At 0 our
	// break-in sat on the edge and was refused whenever the packet timing
	// came out a few samples late (RF bench, 2026-10-14).
	float callee_bk_adjust_s = -0.0025f;
	float first_psk_adjust_s = 0.0f;               // calibration of the caller's first PSK packet
	bool compress = true;                          // online compression (status bits 2-4) when it pays
	bool allow_pmc = true;                         // PMC types too (host MODE 2); MODE 1 = Huffman only (reference manual)
	bool inline_status = true;                     // link up/down also as "*** ..." lines in the RX text
	bool allow_long = true;                        // send long packets when asked (R)
	// P3 (2400 Bd level only, no speed changes yet): offer/accept it in
	// the P1 set-up; the callee sends p3_greeting in its first P3 packet.
	bool allow_p3 = false;
	bool p1_only = false;                         // MYLevel 1: P1 links only (calls placed and answered in P1)
	bool callee_cs4 = true;                       // fix 180: answer a call whose 200 Bd part we cannot read with CS4 (env P2_CALLEE_CS4)
	// Our audio round trip (what we hear is late by the RX latency, what we
	// send leaves late by the TX latency): the callee's P1 replies go out this
	// much early. The P3 caller tolerates only ~4 ms on them. Not derivable
	// from the "p1 reply ... at +X s" log line alone: X also contains the
	// peer's own audio/radio delay (tardis: X - 0.999 = 0.060, yet 0.02-0.03
	// worked and 0.04 failed). Calibrate by a sweep; see the operating notes.
	float rt_latency_s = 0.0f;
	// Fix 243: samples by which the transmitter keys after our audio starts (the host keys the
	// radio when our audio reaches the sound card, less a fixed lead, when the playback path is
	// longer than the default one). What we hear is blanked over the same interval moved by
	// this much, so an answer still arriving before the radio keys is heard. 0: blanked as we
	// transmit (the old way, and the default path).
	int rx_mute_delay = 0;
	int p3_level = 5;                              // 1: as callee ask for level 1 (answer P), as caller send level 1
	std::string p3_greeting = "\r\nSignum P3 station\r\n";
	// Break-in as receiver and "over" as sender (owner decision a): PDuplex / PDTimer, every mode (P1 through
	// P1Config::pduplex, P2/P3 in P2Station::auto_break_in / over_due).
	bool pduplex = false;
	int pdtimer_s = 12;
};

namespace p2arq {
// P1 FSK control signals (12 bits, LSB first) and the P2 acceptance code.
constexpr uint16_t kP1Cs1 = 0x4D5, kP1Cs2 = 0xAB2, kP2Accept = 0x6A9;
constexpr uint16_t kP1Cs4 = 0xD2C;             // answer to a call: link at 100 Bd (the 200 Bd part graded badly)
// PSK control signals: 21 phase steps after the reference pulse (0 = step
// of 1/8 turn (nibble 2), 1 = step of 5/8 turn (nibble a)).
enum PskCs { CS_NONE = -1, CS_P = 0, CS_Q, CS_R, CS_S, CS_T, CS_COUNT };
constexpr const char *kCsPattern[CS_COUNT] = {
	"2aa2a2a2a2aa2a22a2a22",   // P
	"2a222a2aaa2a2a2a2a2a2",   // Q
	"a2aa2a2a22a2aa2a22a22",   // R
	"a2a222aa2a2aa2a2a22a2",   // S
	"a22aaa22aa2222a2aaa22",   // T
};
constexpr const char *kCsName = "PQRST";
// Break-in packet header (words 1..24).
constexpr const char *kBkPattern = "2a2aa2a222a2a2aa2a2a2222";
// Long-packet headers (words 1..8) [level][counter parity]. All are the
// reference unit's level/toggle codewords (long packets), one nibble
// per pulse; level 1 derived from it , equal to the header of the
// bit-exact P3 SL1 long template.
inline constexpr uint8_t kLongHeader[5][2][8] = {
	{},
	{{0xee, 0x6e, 0xee, 0x66, 0xee, 0x66, 0x26, 0x6a}, {0x26, 0xae, 0xee, 0xee, 0x2a, 0x66, 0xa6, 0xe2}},
	{{0x26, 0x2a, 0x6a, 0xe6, 0x26, 0xa6, 0x62, 0x6a}, {0x66, 0x62, 0xe2, 0xe6, 0x6a, 0x2e, 0x6a, 0x6a}},
	{{0xe6, 0xee, 0xa6, 0x26, 0xe6, 0x2a, 0x2a, 0xa2}, {0x2e, 0x2e, 0xa6, 0xae, 0x22, 0x2a, 0xaa, 0x2a}},
	{{0x2e, 0xaa, 0x22, 0xa6, 0x2e, 0xea, 0x6e, 0xa2}, {0x6e, 0xe2, 0xaa, 0xa6, 0x62, 0x62, 0x66, 0xa2}},
};
inline constexpr uint8_t kLongTail[5] = {0, 0xa2, 0x6e, 0x64, 0x10};   // level 1: the P3 SL1 long template's tail [inferred]
// Break-in packet tails by level (level 1 unknown).
inline constexpr uint8_t kBkTail[5] = {0xaa, 0xaa, 0xaa, 0xea, 0x1b};
}  // namespace p2arq

class P2Station {
	friend class P2Monitor;                           // monitor mode (monitor.hpp): a passive receiver
public:
	explicit P2Station(const P2Config &c);

	// Long path (manual 6.22.1, "C !CALL" or "C ;CALL"; P2 / -3 descriptions): the
	// link's cycles are 1.4 s and 4.2 s instead of 1.25 s and 3.75 s. Measured on the
	// emulated V.4.1r (2026-10-23, the long-path captures, the provenance record) [bb-emu]: the master
	// (caller) keeps a 1.4 s lattice; everything the slave does relative to the master's
	// bursts is unchanged (it answers 0.879 s after a short packet's start), so the extra
	// 0.15 s is the master's wait after the slave's burst; after a long packet the slave
	// answers (4.2 - 1.4) s + 0.879 s after its start, 0.30 s later than on a short path.
	// Every cycle-relative time below is written in cyc_s() / lcyc_s(), so these rules hold.
	bool long_path_ = false;
	double cyc_s() const { return long_path_ ? 1.4 : 1.25; }
	double lcyc_s() const { return long_path_ ? 4.2 : 3.75; }
	bool long_path() const { return long_path_; }
	// When the slave hears the master's answer (its lattice + 0.6 s): 0.971 s after the slave's
	// burst started, 0.15 s later on a long path (1.121 s measured on the reference firmware).
	double master_seen_s() const { return 0.971 + (cyc_s() - 1.25); }

	// ---- user interface ---------------------------------------------------
	void connect(const std::string &call);
	static bool p1_own_call() { static const bool o = std::getenv("P1_OWN_CALL") != nullptr; return o; }
	void send_text(const std::string &t);
	void qrt() { if (state_ == ST_P1) p1_->qrt(); else want_qrt_ = true; }
	// The break-in / changeover key and host %O / %I (P1Station::changeover_key, the same keys in every
	// mode). P2/P3 as the reference unit (owner decision a): KEY_BK as receiver breaks in at the next good
	// packet, with or without text waiting (the reference unit's over flag); KEY_CHO as sender sets "over" on
	// our packets once the text typed so far has gone (status bit 6). Returns false when there is nothing to do.
	bool changeover_key(int keys);
	void set_pduplex(bool on, int timer_s) { cfg_.pduplex = on; cfg_.pdtimer_s = timer_s; if (p1_) p1_->set_pduplex(on, float(timer_s)); }
	bool pduplex() const { return cfg_.pduplex; }
	int pdtimer() const { return cfg_.pdtimer_s; }
	// Drop the link at once, without a QRT handshake (host mode DD).
	void abort_link() { if (state_ == ST_UNPROTO) { tx_.clear(); unproto_end("aborted (DD)"); } else if (state_ == ST_P1) p1_end("aborted"); else if (state_ != ST_IDLE && state_ != ST_LISTEN) disconnect("aborted"); }
	// Unproto (manual 6.102). Modes 3-6: P2 short-cycle DPSK at levels 1-4 (100,
	// 200, 400, 800 Bd), each packet `reps` times (1-5; carriers swapped on every repeat) on the
	// 0.8875 s grid, as a bench tool sends it (the reference firmware's listen prints
	// it). Modes 1-2: P1 FSK at 100 / 200 Bd as the real reference unit sends it ([bb-ext]
	// 2026-10-22, the provenance record): one continuous transmission, a packet every 1.0125 s, each
	// copy with the shift inverted, the 52.5 ms between them the next copy's mark tone; header
	// AA/55 alternating per new packet, status = the counter (ASCII, type 0), idle first.
	// First an idle packet, then the text as it comes (send_text), idle packets while there
	// is none; qrt() ends it once the text is out, abort_link() at once, and 4 minutes of idle
	// packets end it too (the firmware's rule). Modes 7-10: P2 long packets at levels 1-4
	// (the ARQ long packet: 0x6a, the long header, 40 pulses a row, its tail), 3.305 s each on
	// a 3.3875 s grid, as the real reference unit sends them ([bb-ext] 2026-10-22: its U 7-10 timed).
	bool start_unproto(int mode, int reps);
	bool unproto() const { return state_ == ST_UNPROTO; }
	void set_debug(bool d) { debug_ = d; }
	// Host-mode view: link events (up/down, with the remote call), bytes not
	// yet sent, bytes in the packet awaiting acknowledgement, set-up in progress.
	struct LinkEvent { bool up; std::string call; };
	std::deque<LinkEvent> take_link_events() { std::deque<LinkEvent> e; e.swap(link_events_); return e; }
	size_t tx_unsent() const { return state_ == ST_P1 ? p1_->tx_unsent() : txq_.size() - size_t(std::max(0, pending_used_)); }
	size_t tx_unacked() const { return state_ == ST_P1 ? p1_->tx_unacked() : size_t(std::max(0, pending_used_)); }
	size_t tx_backlog() const { return tx_unsent(); }   // host status L "not transmitted" (another modem)
	bool setting_up() const { return state_ == ST_CALLING || state_ == ST_SETUP_M || state_ == ST_SETUP_S || state_ == ST_P3SETUP || (state_ == ST_P1 && p1_->calling()); }
	bool qrt_pending() const { return state_ == ST_P1 && p1_ ? p1_->qrt_pending() : want_qrt_; }   // a D (orderly disconnect) in progress
	bool link_up() const { return link_up_; }       // from "connected" to "disconnected", changeovers included
	int watch_fires() const { return watch_fires_; }   // times the link watchdog found the link stalled (link_watch)
	// Self-timing (the real-time audio loop): the callee reply instants from the measured
	// round trip. Applied between links only (a change mid-link would move
	// the replies the peer has locked to).
	bool set_timing(float rt_latency_s, float reply_adjust_s)
	{
		if (link_up_ || setting_up()) return false;
		cfg_.rt_latency_s = rt_latency_s; cfg_.reply_adjust_s = reply_adjust_s;
		if (p1_) p1_->set_rt_latency(rt_latency_s);
		return true;
	}
	// Fix 219: the audio layer saw our round trip move (an overrun resync or a playback restart in
	// the audio loop; a slip test channel): for 30 s the P3 receiver widens its search after misses (p3_search_us).
	// Not otherwise: in noise alone the wider search changed what the 30 decode (p3-rx-fade 4,106 ->
	// 3,899, p3-rx-stepdown 3,756 -> 4,063).
	void note_audio_slip() { slip_hint_n_ = n_; if (p1_) p1_->note_audio_slip(); }   // fix 234: the P1 link too
	// Self-calibration (autocal.hpp): the callee's clear P1 answers to our call and set-up packets
	// (fix 207's track; s after our first bit), and our role in the link.
	int p1_answers(double &mean_s, double &spread_s) const
	{
		if (p1_trk_n_ <= 0) return 0;
		mean_s = p1_trk_sum_ / p1_trk_n_ / cfg_.fs; spread_s = (p1_trk_hi_ - p1_trk_lo_) / cfg_.fs;
		return p1_trk_n_;
	}
	bool master() const { return master_; }
	bool sending() const { return state_ == ST_P1 ? p1_->sending() : state_ == ST_ISS || state_ == ST_BK_SENT; }
	double cycle_s() const { return cyc_s(); }
	// Fix 231c: the same, with what the audio layer did: ms of silence added to the playback queue
	// (+) or of playback not written (-) to hold the round trip. What we hear then lands that much
	// EARLIER against our own transmissions (a capture slip, fix 219), so as callee our packets must
	// move by -ms to stay on the caller's grid; callee_jump follows a jump only toward that.
	void note_audio_slip(double ms)
	{
		note_audio_slip();
		if (rt_rep_n_ < 0 || n_ - rt_rep_n_ > samples(kRtReportS)) rt_pend_ = 0;
		rt_pend_ -= ms * cfg_.fs / 1000.0;
		rt_rep_n_ = n_;
	}
	float rt_latency() const { return cfg_.rt_latency_s; }
	float reply_adjust() const { return cfg_.reply_adjust_s; }
	const std::string &remote() const { return remote_; }
	void set_mycall(const std::string &c) { cfg_.mycall = c; }
	const std::string &mycall() const { return cfg_.mycall; }
	// Terminal/host settings: MODE (0 = ASCII only, else compress when it
	// pays) and MYLevel 3 (offer/accept P3).
	void set_compress(bool on) { cfg_.compress = on; }
	// Fix 241: as P3 receiver, ask for long packets (adaptive in real time) or never (the user's "off")
	void set_p3_long(bool on) { cfg_.p3_request_long = on; }
	bool p3_long() const { return cfg_.p3_request_long; }
	// CPU fallbacks (scs-modem TODO 5, the same CPUadapt setting): the host's CPU health (src/cpu_health.hpp:
	// 0 OK, 1 BUSY, 2 STARVED), given on every poll. While it is not OK, and for kCpuFbHoldS after, with
	// CPUadapt on, CPU-bound extras are left out (degrade, never fail): P3 decodes only the expected level
	// while packets decode at it (p3_fb_steady). cpu_fallbacks(): the ones applied in the last kCpuFbHoldS
	// (FB_* bits), for the host to report.
	enum { FB_P3_LEVELS = 1 };
	void set_cpu_health(int s)
	{
		cpu_health_ = s;
		if (s > 0) cpu_fb_until_ = std::max(cpu_fb_until_, n_ + samples(kCpuFbHoldS));
	}
	int cpu_fallbacks() const
	{
		int m = 0;
		for (int k = 0; k < 1; k++) if (fb_at_[k] >= 0 && n_ - fb_at_[k] < samples(kCpuFbHoldS)) m |= 1 << k;
		return m;
	}
	int p3_long_ceiling() const { return p3l_ceil_; }   // the highest level long packets are asked for in real time (0: none)
	void set_p3_long_ceiling(int c) { p3l_ceil_ = std::max(0, std::min(6, c)); }   // a host's new station keeps what the old one measured
	void set_tx_mode(int m) { cfg_.compress = m != 0; cfg_.allow_pmc = m >= 2; }   // host MODE 0/1/2
	int tx_mode() const { return !cfg_.compress ? 0 : cfg_.allow_pmc ? 2 : 1; }
	float tone_lo() const { return cfg_.tone_lo; }
	float tone_hi() const { return cfg_.tone_hi; }
	bool compress() const { return cfg_.compress; }
	void set_allow_p3(bool on) { cfg_.allow_p3 = on; }
	void set_p1_only(bool on) { cfg_.p1_only = on; }   // MYLevel 1
	bool p1_only() const { return cfg_.p1_only; }
	bool allow_p3() const { return cfg_.allow_p3; }
	bool caller() const { return master_; }         // we called (valid while/after a link)
	// Answer incoming calls (default) or not: with OFDM answering only, or while
	// the OFDM modem has the link (a multi-modem station).
	// Our connect text as callee (the firmware's "CMSG": LinBPQ turns it off,
	// and its BBS then speaks first). Empty: the first P3 packet is idle.
	void set_greeting(const std::string &g) { cfg_.p3_greeting = g; }
	void set_answer(bool on)
	{
		answer_ = on;
		if (!on && state_ == ST_LISTEN) state_ = ST_IDLE;
		else if (on && state_ == ST_IDLE) state_ = ST_LISTEN;
	}
	int link_plevel() const { return p1_link_ ? 1 : p3_ ? 3 : 2; }   // level of the current/last link
	// Why the last link ended (" (timeout)", " (QRT received)", ...; "" while up)
	const std::string &link_note() const { return link_note_; }
	// Decode margin (the design notes 2): for each received data packet decoded on
	// a worker (P2, P3 short and long), how long before its deadline (our answer
	// due) the decode was done, in ms; late = not done in time (the packet then
	// counts as failed). Per link (reset at link up). Real time only.
	struct DecodeStats {
		long n = 0, late = 0, late_sl1 = 0;       // margins measured; late main decodes; late SL1 fallback tries (P3)
		long late_bk = 0;                         // late break-in decodes (we were sending: the peer's break-in answered blind)
		double last_ms = NAN, min_ms = NAN, sum_ms = 0;
		int last_level = 0;                       // the speed level the last one was decoded at
		std::array<float, 32> recent{};           // the last 32 margins (ms; late: -1), ring
		int rn = 0;
		double median_recent() const
		{
			const int k = int(std::min<long>(n + late, long(recent.size())));
			if (k == 0) return NAN;
			std::array<float, 32> c = recent;
			std::sort(c.begin(), c.begin() + k);
			return c[size_t(k / 2)];
		}
	};
	const DecodeStats &decode_stats() const { return dstats_; }
	// Load counters for the host's CPU health indicator (src/cpu_health.hpp): cumulative since this
	// station was built (never reset by a link), real-time (paced) runs only. The host polls them and
	// works out the rest; the modem only counts (no clock, no OS calls beyond what it already uses).
	struct LoadCounters {
		long late = 0;          // main decodes not done in time (note_late)
		long late_bk = 0;       // late break-in decodes
		long thin = 0;          // decodes done with less than kThinMs to spare
		long caps = 0;          // computer back-off steps (the P3 receive level capped lower)
		long hiccups = 0;       // late or thin decodes treated as a hiccup (fix 264: no back-off for them)
		long fallbacks = 0;     // adaptive features switched off by level (long packets)
		long fb_p3_levels = 0;  // CPU fallback applied (TODO 5): P3 packets tried at the expected level only
		long sl1_rescue = 0;    // packets taken from the SL1 try because the main decode was late
		long tx_late = 0;       // transmit bursts whose audio was not ready at their start
		long look_wait = 0;     // P3 control-signal looks waited for before our next burst (fix 250)
		double last_thin_ms = NAN;   // the margin of the last thin decode
		double last_tx_wait_ms = 0;  // how long the last late transmit audio was waited for
		int last_late_level = 0;     // the speed level of the last late decode
	};
	const LoadCounters &load_counters() const { return load_; }
	// Back-off (the design notes 3): the computer throttling now (the real-time audio loop, from
	// sysmon every 2 s); the P3 receive level cap in force (6: none)
	void set_computer_throttled(bool t) { throttled_ = t; }
	int p3_level_cap() const { return p3_cap_; }
	// Front panel statistics of the current/last link (counted at single
	// points, no protocol logic): control signals sent and classified per
	// letter (P Q R S T), text bytes received / queued by the host, link start.
	// pk_ok / pk_err / pk_rep: the packet checks of this link (P2/P3), as the Idle/Request and
	// Traffic/Error LEDs show them: good, nothing decoded (error), a repeat (received again, or
	// asked for again); q_hist: the last 16, newest in bit 0, 1 = good; q_n: how many so far.
	struct PanelStats { std::array<int, 5> cs_tx{}, cs_rx{}; size_t rx_bytes = 0, tx_text = 0; int64_t t_up = -1;
	                    int pk_ok = 0, pk_err = 0, pk_rep = 0, q_n = 0; unsigned q_hist = 0; };
	const PanelStats &panel_stats() const { return pstats_; }
	// Link quality for the front panel: the share of good checks among the last 16, in percent
	// (-1: none yet this link, or a P1 link, which does not count them).
	int link_quality() const
	{
		const int n = std::min(16, pstats_.q_n);
		if (!link_up_ || state_ == ST_P1 || n == 0) return -1;
		return (100 * __builtin_popcount(pstats_.q_hist & ((1u << n) - 1u)) + n / 2) / n;
	}
	double link_seconds() const { return link_up_ && pstats_.t_up >= 0 ? double(n_ - pstats_.t_up) / cfg_.fs : 0.0; }
	double panel_afc_hz() const { return afc_hz(); }   // tracked frequency offset of the peer
	double p3_rx_shift_hz() const { return -p3_rx_df_; }   // the P3 receive shift, Hz as heard (tests)
	int last_cs_rx() const { return last_rx_cs_; }
	int last_cs_tx() const { return last_tx_cs_; }
	// Extended status (host mode channel 254, "G3"; LinBPQ's
	// driver reads it): mode << 4 | sending << 3 | state, level, speed
	// level, frequency offset (128: unknown). Modes: 0 STANDBY, 2 ARQ;
	// states: 1 REQUEST (calling), 2 TRAFFIC. Layout from LinBPQ's decoder and
	// the reference firmware's answer (F7 00 01 80 in standby), 2026-10-19.
	void host_status(uint8_t out[4]) const;
	// Front panel: speed level of the current link (P2 1-4, P3 SL 1-6; 0 =
	// none) and the data type of the last received data packet (status bits 2-4).
	int speed_level() const { return !link_up_ ? 0 : state_ == ST_P1 ? (p1_->link_baud() == 200 ? 2 : 1) : p3_ ? p3_level_ : level_; }
	// Front panel, one row per direction (owner, 2026-11-22: "a TX row with speed LEDs and an RX row"): the level
	// this station sends at and the level it expects the other station to send at, and long or short packets each
	// way. P3 keeps the two apart (p3_level_ / p3_rx_level_); P2 has one level_, set to rx_level_ while receiving,
	// so its TX row shows the level last sent at or now expected. P1: the link's baud rate both ways, no long packets.
	int rx_speed_level() const { return !link_up_ ? 0 : state_ == ST_P1 ? speed_level() : p3_ ? p3_rx_level_ : std::max(1, rx_level_); }
	bool tx_long_packets() const { return link_up_ && state_ != ST_P1 && (p3_ ? p3_long_ : long_); }
	bool rx_long_packets() const { return link_up_ && state_ != ST_P1 && (p3_ ? p3_rx_long_ : rx_long_); }
	// Front panel (owner, 2026-11-22): what the receiver hears (rx_meter.hpp: the S/N during a link, the audio
	// level always), and how the far end hears us: the meaning of the last control signal it sent back while we
	// send (FAR_* bits, 0 = none or not sending; kept for two long cycles: a missing answer darkens it at once).
	const RxMeter &rx_meter() const { return meter_; }
	bool rx_snr(double &db) const { return link_up_ && meter_.snr(db); }
	enum { FAR_OK = 1, FAR_REPEAT = 2, FAR_FASTER = 4, FAR_SLOWER = 8, FAR_LENGTH = 16 };
	int far_end() const
	{
		if (!link_up_) return 0;
		if (state_ == ST_P1) return p1_ && p1_->sending() ? p1_->far_end() : 0;
		return (state_ == ST_ISS || state_ == ST_BK_SENT) && far_n_ >= 0 && n_ - far_n_ < samples(2 * lcyc_s()) ? far_ : 0;
	}
	// Front panel (mode lamps): the P1 phase of a call (calling or
	// setting up) runs at 200 Bd, or 100 Bd after a CS4 answer.
	bool p1_phase() const { return !link_up_ && setting_up() && state_ != ST_P3SETUP; }
	int p1_baud() const { return p1_slow_ ? 100 : 200; }
	int last_rx_type() const { return last_rx_type_; }
	// Front-panel LEDs as seen on the reference unit. Each per-cycle result holds
	// until the next packet/acknowledgement check, i.e. one cycle.
	// Idle/Request: 1 green = the packet (received new / being sent) holds
	// idle fill, 2 red = a repeat (received again / asked for again).
	// Traffic/Error: 1 green = it carries data, 2 red = nothing decoded (bad
	// CRC / no acknowledgement).
	int led_idle_req() const { return link_up_ ? led_ir_ : 0; }
	int led_traffic_err() const { return link_up_ ? led_te_ : standby_status() == 0xF7 ? 1 : 0; }   // standby: green = channel busy
	int led_marq() const { return link_up_ ? led_marq_ : 0; }
	// Send green: we are the sender (the whole time, not only while keyed;
	// also while calling). CHO red: from a change of direction until the
	// first good packet or acknowledgement in the new direction.
	bool led_send() const { return state_ == ST_ISS || state_ == ST_CALLING; }
	bool p1_sending() const { return state_ == ST_P1 && p1_ && p1_->sending(); }   // the panel's Send lamp on a P1 link
	bool led_cho() const { return link_up_ && led_good_role_ >= 0 && led_good_role_ != (state_ == ST_ISS ? 1 : 0); }
	// Tracking green: a frequency correction was applied this cycle.
	bool led_tracking() const { return link_up_ && led_trk_n_ >= 0 && n_ - led_trk_n_ < samples(cyc_s()); }
	// Compress: the data type of the packet being sent (as sender) or the
	// last received one (status bits 2-4; 4-7 PMC, 1-2 Huffman).
	int panel_data_type() const { return state_ == ST_ISS ? led_tx_type_ : last_rx_type_; }
	// Channel busy (busy_detector.hpp): the standby status byte, 0x87 free / 0xF7 busy,
	// 0xE7 in Listen mode (no busy given there). busy_lamp(): the same detector in any
	// standby (Listen too): 1 busy, 0 free, -1 not evaluated (link).
	void set_listen(bool on) { listen_cmd_ = on; }
	// Fix 269: the other station is a Signum (its marker or encryption record read on this link; the
	// core sets it from src/peer_id.hpp every audio period, false while not known).
	void set_peer_signum(bool on) { peer_signum_ = on; }
	bool peer_signum() const { return peer_signum_; }
	bool p3_late_whole() const { return late_psk_whole(); }   // a late short P3 burst goes whole (fix 257) now
	uint8_t standby_status() const { return !standby() ? 0 : listen_cmd_ ? 0xE7 : busy_.busy() ? 0xF7 : 0x87; }
	int busy_lamp() const { return !standby() ? -1 : busy_.busy_lamp() ? 1 : 0; }
	// New FSK tone pair (P1 phase and P2 PSK centre); only while no link is up.
	void set_tones(float lo, float hi)
	{
		if (state_ != ST_IDLE && state_ != ST_LISTEN) return;
		cfg_.tone_lo = lo; cfg_.tone_hi = hi;
		disc_ = decltype(disc_)(cfg_.fs, (lo + hi) / 2, hi - lo);
		busy_.set_tones(lo, hi);
	}
	bool connected() const { return state_ >= ST_ISS; }
	bool idle() const { return state_ == ST_IDLE; }
	bool ptt() const { return ptt_; }
	int level() const { return level_; }
	std::string take_rx() { std::string s; s.swap(rx_); return s; }
	// Signum's own SIs received on a P2/P3 link (function code 'X', compress.hpp SiFilter): each body with
	// its position in the text take_rx() returns; call it right after take_rx(), on the same thread.
	struct SiMsg { size_t pos; std::string body; };
	std::vector<SiMsg> take_si() { std::vector<SiMsg> v; v.swap(si_rx_); return v; }
	// Sends one of our SIs (0x1C, 'X', body, space) at this point of the text stream, unescaped, so that the far end's
	// SI layer reads it (a reference unit skips it). The body: 0x21-0x7E only, never 0x1C-0x1E or a
	// space (the caller's rule). P2/P3 links only (never P1: other P1 equipment may print it).
	void send_si(const std::string &body)
	{
		if (state_ == ST_P1) return;
		txq_ += rle_tx_.flush();                       // a held run first, so the SI follows it in the stream
		txq_ += '\x1c'; txq_ += 'X'; txq_ += body; txq_ += ' ';
		if (state_ == ST_ISS && !premod_.empty()) premod_stale_ = true;
	}
	std::string take_log() { std::string s; s.swap(log_); return s; }

	int16_t tick(int16_t rx_sample);

private:
	enum State { ST_IDLE, ST_LISTEN, ST_CALLING, ST_SETUP_M, ST_SETUP_S, ST_P3SETUP, ST_UNPROTO, ST_ISS, ST_IRS, ST_BK_SENT, ST_P1 };
	enum EvType { EV_P1_CS, EV_P1_PKT, EV_P1_AGAIN, EV_REPLY, EV_BK_DONE, EV_PACKET, EV_P3, EV_P3_LAUNCH, EV_BK_DATA, EV_P3_SYNC, EV_P2_SYNC, EV_P3_SL1, EV_P2_LAUNCH, EV_BK_AGAIN, EV_P3L_LAUNCH, EV_UNPROTO, EV_P3L_SYNC, EV_P3L_PRE, EV_P2L_PRE };
	struct Event { int64_t at; EvType type; int64_t ref; int arg; };
	// fut: audio still being made on a worker (play() takes it at `start`)
	// P2_TX_PROF=1 (test): when a burst made on a worker was submitted, began and ended (steady clock, ms),
	// logged when the audio loop had to wait for it (P2 bursts, and P3 data bursts since fix 273)
	struct TxTrace { std::atomic<double> sub{0}, beg{0}, end{0}; std::atomic<int> khz{0}; int64_t queued_n = 0; bool hit = false; };
	static int cur_khz() { int v = 0; if (FILE *f = std::fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "r")) { if (std::fscanf(f, "%d", &v) != 1) v = 0; std::fclose(f); } return v; }
	static double now_ms() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
	static bool tx_prof() { static const bool on = std::getenv("P2_TX_PROF") != nullptr; return on; }
	template <class F> static auto traced(std::shared_ptr<TxTrace> tr, F f) { tr->sub = now_ms(); return [tr, f]() { if (tx_prof()) tr->khz = cur_khz(); tr->beg = now_ms(); auto r = f(); tr->end = now_ms(); return r; }; }
	// A P2 long packet made in pieces on a worker (p2::modulate_range; scs-modem TODO: the one-core
	// sender): its first kHead samples first, then kChunk at a time; `done` says how far it is. The
	// audio loop plays (and shifts) what is done and waits only for a sample not yet made.
	// P2_TX_PIECES_OLD=1: made whole (the audio loop waits for all of it at its start), as before.
	struct TxProg {
		static constexpr size_t kHead = 480, kChunk = 2048;   // 50 ms, then 213 ms
		p2::ModPlan plan;
		std::vector<float> y;
		std::atomic<size_t> done{0};
		std::mutex m;
		std::condition_variable cv;
		void make()
		{
			for (size_t a = 0; a < y.size();) {
				const size_t b = std::min(y.size(), a + (a == 0 ? kHead : kChunk));
				p2::modulate_range(plan, y.data(), a, b);
				if (test_us() > 0) std::this_thread::sleep_for(std::chrono::microseconds(int64_t(test_us() * double(b - a) / 256.0)));
				{ std::lock_guard<std::mutex> lk(m); done.store(b, std::memory_order_release); }
				cv.notify_all();
				a = b;
			}
		}
		size_t ready() const { return done.load(std::memory_order_acquire); }
		// test hook: P2_TEST_TX_US=U, a P2 long packet's audio made
		// as slowly as on a slow core: U microseconds more per 256 samples made (600: ~75 ms a packet)
		static int test_us() { static const int v = std::getenv("P2_TEST_TX_US") ? std::atoi(std::getenv("P2_TEST_TX_US")) : 0; return v; }
		void wait(size_t n) { std::unique_lock<std::mutex> lk(m); cv.wait(lk, [&] { return done.load(std::memory_order_acquire) >= std::min(n, y.size()); }); }
	};
	static bool tx_pieces_on() { static const bool old = std::getenv("P2_TX_PIECES_OLD") != nullptr || std::getenv("TX_TEST_STEP") != nullptr; return !old; }
	struct TxBurst {
		int64_t start; std::vector<float> audio; std::shared_future<std::vector<float>> fut; bool shifted = false; std::shared_ptr<TxTrace> tr;
		std::shared_ptr<TxProg> prog;                   // made in pieces (audio unused)
		double df = 0;                                  // prog: its shift (chosen at its start)
		std::vector<float> sy; long sdone = 0;          // prog with a shift: the shifted samples, made up to sdone
		size_t len() const { return prog ? prog->y.size() : audio.size(); }
	};
	// Fix 253: a burst queued after its start never sends its first part (play() is at n_): counted
	// and logged (in real time always, as a late TX audio; lockstep with --debug), so a look that
	// decides too late shows.
	int tx_cut_ = 0;
	void tx_push(TxBurst &&tb)
	{
		if (tb.start < n_) {
			tx_cut_++;
			if (debug_ || cfg_.realtime) { char b[96]; std::snprintf(b, sizeof b, "TX burst queued %.1f ms after its start: its first part not sent", 1e3 * double(n_ - tb.start) / cfg_.fs); log(b); }
		}
		tx_.push_back(std::move(tb));
	}
	// ~13.6 s at 9600 Hz. A memory-starved build (esp32/, no PSRAM) may set
	// SIGNUM_P2_RING_LOG2 lower; history older than the ring is then unavailable.
#ifndef SIGNUM_P2_RING_LOG2
#define SIGNUM_P2_RING_LOG2 17
#endif
	static constexpr int64_t kRing = int64_t(1) << SIGNUM_P2_RING_LOG2;

	int64_t samples(double s) const { return int64_t(std::llround(s * cfg_.fs)); }
	void log(const std::string &s) { log_ += "[pt2] " + s + "\n"; }
	void schedule(int64_t at, EvType t, int64_t ref, int arg = 0);

	// ======================= P1 FSK (link set-up) ===========================
	double bit_soft(double a, int baud) const;
	void bits_at(double first, int baud, int nbits, bool inv, uint8_t *out) const
	{
		for (int k = 0; k < nbits; k++) out[k] = uint8_t((bit_soft(first + k * cfg_.fs / baud, baud) > 0 ? 1 : 0) ^ (inv ? 1 : 0));
	}
	static void pack(const uint8_t *bits, int n, uint8_t *bytes)
	{
		std::memset(bytes, 0, size_t((n + 7) / 8));
		for (int i = 0; i < n; i++) if (bits[i]) bytes[i / 8] |= uint8_t(1u << (i % 8));
	}
	// Best 12-bit FSK code among `codes` with first bit in [a, b].
	// Power at the two FSK tones against two tones outside the signal, in dB,
	// over n samples from t0 (Goertzel on the received audio).
	double p1_tone_snr_db(int64_t t0, int n) const;
	// The answer search's bits from running sums (Signum, 2026-11-13). bit_soft's correlation of
	// a 100 Bd bit with the two tones, done directly, cost the search 12 bits x 88 samples per
	// place: about 80 ms of one audio period on an x86 build machine for the nominal tones and the
	// offset grid of an unanswered call, which drained the playback queue (40 ms) once per call
	// cycle and cut the start of our next call packet (on the air: one playback underrun per cycle,
	// 27.5 ms dropped). Here the samples times each tone are summed once over the search span
	// (ps_hi_, ps_lo_, allocated at construction), and a bit is the difference of two sums: the
	// same value as bit_soft (only the phase reference differs, which the energies do not see).
	// Off (the direct way) for P2_P1_DISC / P2_P1_TONE, a span longer than the buffer, or
	// P1_SEARCH_DIRECT=1.
	bool p1_tone_sums(int64_t a, int64_t b) const;
	double p1_bit_fast(double a) const;
	mutable std::vector<std::complex<double>> ps_hi_, ps_lo_;
	mutable int64_t ps_t0_ = 0, ps_t1_ = -1;
	double find_p1_code(int64_t a, int64_t b, const std::vector<uint16_t> &codes, uint16_t &code, int64_t &at) const;
	static std::vector<uint8_t> call200(const std::string &call);
	// An FSK burst: `segs` of (baud, bits); first bit at `first`.
	void queue_fsk(int64_t first, const std::vector<std::pair<int, std::vector<uint8_t>>> &segs);
	// Callee: our P2-/P3-accept at `first`. Test hook (fix 218): P2_TEST_ACCEPT_MUTE=N /
	// P3_TEST_ACCEPT_MUTE=N: our first N accepts go out silent (sent, so the P1 polarity still
	// alternates, but nothing heard: a caller that loses them, for the bench).
	void queue_accept(int64_t first, uint16_t code);
	static void push_bits(std::vector<uint8_t> &v, const uint8_t *bytes, int nbits)
	{
		for (int i = 0; i < nbits; i++) v.push_back(uint8_t(bytes[i / 8] >> (i % 8) & 1));
	}
	static std::vector<uint8_t> code_bits(uint16_t c)
	{
		std::vector<uint8_t> v;
		for (int i = 0; i < 12; i++) v.push_back(uint8_t(c >> i & 1));
		return v;
	}
	void send_call(int64_t first);
	// The caller's P1 data packet that asks for P2 (status 0x11).
	void send_p1_setup(int64_t first);
	// Callee: a call to us whose first bit started 72+48 bits ago?
	// Fix 185: the call is looked for on a grid of frequency offsets (0, +-50, +-100 ...
	// up to p1_acq_max_hz(), the non-zero ones every 16th sample), the tones' correlation over a
	// bit (bit_soft) moved by each. On the nominal tones alone a call 100 Hz off puts its low
	// tone half-way between ours: its 0 bits read at random. P1_ACQ_MAX_HZ=0: the old search.
	static double p1_acq_max_hz() { static const double v = std::getenv("P1_ACQ_MAX_HZ") ? std::atof(std::getenv("P1_ACQ_MAX_HZ")) : kP1AcqMaxHz; return v; }
	static constexpr double kP1AcqMaxHz = 100.0;
	void acquire_call();
	// Test hook (fix 187's measurement): P2_TEST_CALL_ANSWER="2,2,0,1": what we answer the
	// calls to us we hear, in turn (2 = CS2 "busy", 4 = CS4, 0 = nothing, 1 or the list used up:
	// as normal). A hooked answer leaves us listening; the same call is not taken again for 1.2 s.
	int64_t acq_hold_until_ = 0;
	size_t test_answer_i_ = 0;
	int test_call_answer()
	{
		const char *e = std::getenv("P2_TEST_CALL_ANSWER");
		if (!e) return 1;
		std::vector<int> v;
		for (const char *q = e; *q; ) { v.push_back(std::atoi(q)); const char *c = std::strchr(q, ','); if (!c) break; q = c + 1; }
		return test_answer_i_ < v.size() ? v[test_answer_i_++] : 1;
	}
	void acquire_call_at();
	// Fix 180's switch (callee CS4): env P2_CALLEE_CS4=0/1 over P2Config::callee_cs4.
	bool callee_cs4() const { const char *e = std::getenv("P2_CALLEE_CS4"); return e ? std::atoi(e) != 0 : cfg_.callee_cs4; }
	static constexpr uint16_t kP1Cs1() { return p2arq::kP1Cs1; }
	// Callee's P1 replies (CS1, accept codes): first bit this long after the
	// first bit of what it answers. 1.003 s worked for P2, but a P3 caller
	// (emulated firmware) only accepts -6..-2 ms around it and answers B1
	// with S otherwise: 0.999 s (2026-10-13).
	static constexpr double kP1Reply = 0.999;

	// A caller's P1 set-up packet (CRC good) with its first bit near `exp`
	// (+-12 ms): its first bit and bytes.
	// After our CS4 call answer (fix 180) it comes at 100 Bd: 96 bits, 8 data bytes; its status
	// byte is copied to bytes[21], where the callers read it.
	bool p1_setup_at(int64_t exp, int64_t &t_out, uint8_t *bytes) const;
	// Callee: the caller's set-up packet near `exp` (+-12 ms): the first position (2-sample
	// steps) and polarity, in that order, whose packet checks (CRC; at 100 Bd also the header),
	// its first bit and bytes. Fix 202: each position's bits are read once for both polarities
	// (the inverted ones are the same bits flipped), and the positions are split over the compute
	// pool, each part in order; the earliest hit wins, so the result is the one-by-one scan's.
	// It runs inside the one sample that answers the set-up (and as callee of a P1-only
	// caller, hands the link over): one by one it was ~8.5 ms of a 35-41 ms tick on the x86 build machine.
	bool p1_setup_scan(int64_t exp, int baud, int nb, int nd, int64_t &t_out, uint8_t *bytes_out) const;
	// Callee: the caller's P1 set-up packet should have started near `exp`.
	void on_p1_packet(int64_t exp);

	// Caller: what answered our call (arg 0) or our set-up packet (arg 1)?
	void on_p1_cs(int64_t ref, int which);

	// ============================ PSK ========================================
	// Words -> audio burst starting (pulse 0) at `start`. Returns where pulse 0 goes (later than `start`
	// for a late short burst, below).
	int64_t queue_psk(int64_t start, const std::vector<uint8_t> &words, bool may_move = false);
	bool late_psk_whole() const
	{
		static const bool old = std::getenv("P3_LATE_CUT_OLD") != nullptr;
		static const bool always = std::getenv("P3_LATE_WHOLE") != nullptr;
		return cfg_.realtime && !old && (always || peer_signum_);
	}
	void send_cs(int cs, int64_t start);
	int reply_delay_mine() const
	{
		if (p3_ && !master_) return int(samples(kP3CalleeReply - cfg_.rt_latency_s));
		return int(samples(master_ ? 0.971 + (cyc_s() - 1.25) + cfg_.master_adjust_s : 0.879 + cfg_.reply_adjust_s));
	}
	// When we answer the burst that started at `rx_start`. The callee answers
	// 0.879 s after what it hears. The caller answers on ITS OWN grid (its
	// transmit lattice + 0.6 s), so the callee sees the answer 0.971 s after
	// its burst whatever the round-trip delay; answering relative to what we
	// hear would arrive late by the round trip (radio + audio latency).
	int64_t reply_at(int64_t rx_start, bool after_long) const;
	int reply_delay_theirs() const { return int(samples(master_ ? 0.879 : 0.971 + (cyc_s() - 1.25))); }

	// Demodulated burst: pulse amplitudes of both carriers from one pulse
	// before the estimated start of pulse 0.
	struct Demod { bool ok = false; double start = 0; std::vector<std::complex<double>> lo, hi; double c[2] = {0, 0}; };
	// A copy of received samples [t0, end) (zero outside), so a decode can
	// run on a worker thread while the audio loop goes on writing the ring.
	struct Snap {
		std::vector<float> v; int64_t t0 = 0, end = 0;
		float at(int64_t t) const { return t >= t0 && t < end ? v[size_t(t - t0)] : 0.0f; }
	};
	Snap snapshot(int64_t from, int64_t to) const
	{
		Snap sp; sp.t0 = from; sp.end = to;
		sp.v.resize(size_t(std::max<int64_t>(to - from, 0)));
		for (int64_t t = from; t < to; t++)
			sp.v[size_t(t - from)] = (t >= 0 && t < n_ && n_ - t < kRing - 1) ? raw_[size_t(t % kRing)] : 0.0f;
		return sp;
	}
	// The samples find_packet(exp, false, false) reads.
	Snap snapshot_short(int64_t exp) const { return snapshot(exp - samples(0.04) - int64_t(2 * T_), exp + samples(0.86) + int64_t(2 * T_) + 1); }
	// Pulse timing (the first pulse centre on each carrier, samples) from the
	// burst in x, which starts at sample `from`.
	// fshift: the tones moved by the peer's measured offset (fix 206).
	bool timing_of(const std::vector<float> &x, int64_t from, double &c_lo, double &c_hi, double fshift = 0) const;
	// The joint fit's inputs for demod's window [from, to) (its samples from `base`, ysize of them).
	struct LsArgs { int64_t base = 0; size_t ysize = 0; double cA = 0, cB = 0, fA = 0, fB = 0; int n = 0; };
	LsArgs ls_args(int64_t from, int64_t to, double c_lo, double c_hi, double flo, double fhi, int npulses) const
	{
		LsArgs a;
		a.ysize = size_t(to - from) + size_t(4 * T_);
		a.base = from - int64_t(2 * T_);
		a.cA = c_lo - nw_ / 2.0 - T_ - double(a.base); a.cB = c_hi - nw_ / 2.0 - T_ - double(a.base);
		a.fA = flo; a.fB = fhi; a.n = npulses + 2;
		return a;
	}
	Demod demod(int64_t from, int64_t to, int npulses, const Snap *sp = nullptr, const double *cc = nullptr, double fshift = 0) const;
	// Fix 206: control signals (and the other bursts read by their steps: break-ins, set-up
	// words, the QRT end packet) demodulated at the tones moved by the peer's offset. Data
	// packets have been (fix 64, break-ins fix 147), but these were fitted at the nominal tones
	// and their timing (timing_of) mixed there and low-passed at 80 Hz: at 80 Hz and more off,
	// every control signal from the reference unit read at ~0.4 and a P2 link we called sent nothing.
	// The pulses are fitted at the moved tones and then turned back by the offset's phase at
	// each pulse centre, so every step keeps its frame (the offset in it, as afc_ counts it) and
	// the scoring, its priors and the AFC updates are unchanged. afc: in afc_'s units (the
	// worker's copy). P3's control signals and break-ins too (its data packets move their audio
	// instead, fix 49). P2_CS_NO_DEMOD_AFC / P3_CS_NO_DEMOD_AFC: off; P2_CS_AFC_MIN (Hz, default
	// 1, as data packets): the smallest offset acted on.
	double cs_shift_hz(double afc) const;
	// timing_of's mixing follows the demodulation's shift (P2_TIMING_NO_AFC: nominal, as before 206).
	static double timing_shift(double fshift) { static const bool off = std::getenv("P2_TIMING_NO_AFC") != nullptr; return off ? 0.0 : fshift; }
	Demod demod_cs(int64_t from, int64_t to, int npulses, const Snap *sp, const double *cc, double afc) const;
	// The demodulation with one carrier slipped a pulse against the other
	// (v 0: the earlier carrier a pulse later; v 1: the later one a pulse
	// earlier), taken from d's fit by re-indexing: the joint fit already has
	// every pulse of both carriers, so a fresh fit (one more ls_pulses, a
	// third of a failed decode's time) gives the same values but at the
	// burst's edges. P2_SLIP_REDEMOD: fit again (test).
	Demod slipped(const Demod &d, int v, int64_t from, int64_t to, int npulses, const Snap *sp) const;
	// The earlier carrier's grid moved k pulses later (slipped's variant 0 by k pulses; fix 196, classify_reply).
	Demod slip_by(const Demod &d, int k) const;
	static double nib(const std::complex<double> &a, const std::complex<double> &b)
	{
		const double v = -16.0 * std::arg(b * std::conj(a)) / (2 * M_PI);
		return std::fmod(v + 32.0, 16.0);
	}
	// Steps of words 1..n for carrier order (A on lo if !swap) and word-0
	// position o in the demod arrays.
	static void steps_of(const Demod &d, bool swap, int o, int n, std::vector<std::array<double, 2>> &st)
	{
		const auto &A = swap ? d.hi : d.lo, &B = swap ? d.lo : d.hi;
		st.assign(size_t(n), {0, 0});
		for (int w = 1; w <= n; w++) {
			const size_t i = size_t(o + w);
			if (i >= A.size() || i >= B.size()) break;
			st[size_t(w - 1)] = {nib(A[i - 1], A[i]), nib(B[i - 1], B[i])};
		}
	}
	// Amplitude weight of steps first+1..first+n: |z(k-1)| |z(k)| over the
	// median of that product (a step between weak pulses is mostly noise).
	static void step_weights(const Demod &d, bool swap, int o, int first, int n, double (*w)[2]);
	// Agreement of steps 1..len with a 2/a pattern on both carriers (0..1).
	// Fraction of the steps (both carriers) that match a 'a'/'2' pattern.
	// A frequency offset between the two radios adds a constant to every
	// step (RF bench: two IC-7300s drifted up to ~25 Hz apart, 4/16 of a
	// turn per step), so the common offset is estimated first, limited to
	// +-4/16 so that a pattern and its complement (a half turn apart) are
	// not confused, and removed.
	// prior: the offset tracked from earlier packets (afc_), so that the
	// +-4/16 limit is around it (the offset drifted past half a turn per
	// step, 50 Hz, in one RF bench run; P and an inverted Q are then
	// indistinguishable without it).
	static double pattern_bias(const std::vector<std::array<double, 2>> &st, const char *pat, int len, double prior = 0);
	static double pattern_score(const std::vector<std::array<double, 2>> &st, const char *pat, double prior = 0, double *bias_out = nullptr);

	// Identify a control signal in the burst expected to start near `exp`.
	// Front panel: every control signal classified, per letter (find_psk_cs_raw).
	struct ReplyRx {
		bool bk = false; int cs = p2arq::CS_NONE; double start = 0, score = 0, bias = 0;
		// soft decision (fix 62): best letter's mean cos of the phase error, its
		// margin over the next letter, and the timing it was found with
		int soft_cs = p2arq::CS_NONE; double soft = -1, margin = 0, soft_bias = 0; bool soft_tracked = false;
		double c[2] = {0, 0}; bool has_c = false; double soft_start = 0; int64_t exp = 0;
		double hc[2] = {0, 0}; bool has_hc = false;     // the hard decision's own timing
		bool bk_tracked = false; double bk_soft = -1;   // fix 63: a break-in found at the tracked timing
		bool tracked_look = false;                     // the look had a timing prediction (data-phase acknowledgements only)
		bool ctx = false;                              // a trackable look (data-phase acknowledgement): only these teach a track (fix 71)
		int trk = 0;                                   // which track: 0 short cycle, 1 long cycle (fix 72)
		int cross_cs = p2arq::CS_NONE;                 // fix 169: an untracked look's own clear letter, against the tracked soft one
		int wide = 0;                                  // fix 196: the hard reading came from carriers re-aligned by this many pulses
		int heard_steps = 0;                           // fix 257: read from this many steps heard (a late control signal), 0: all
	};
	struct CsPred { bool ok = false; double c[2][2] = {{0, 0}, {0, 0}}; int first = 0; double start = 0; int trk = 0; };
	int find_psk_cs(int64_t exp, double &start, int track = 0)
	{
		const int c = find_psk_cs_raw(exp, start, track);
		if (c >= 0 && c < 5) pstats_.cs_rx[size_t(c)]++;
		return c;
	}
	// Fix 250: the same from a classification already made (on a worker, p3_look)
	int find_psk_cs_from(int64_t exp, const ReplyRx &rr, double &start, int track)
	{
		const int c = find_psk_cs_apply(exp, rr, start, track);
		if (c >= 0 && c < 5) pstats_.cs_rx[size_t(c)]++;
		return c;
	}
	int find_psk_cs_raw(int64_t exp, double &start, int track = 0);   // track: 0 none (set-up), 1 short cycle, 2 long cycle
	int find_psk_cs_apply(int64_t exp, ReplyRx r, double &start, int track);
	// Frequency offset between the stations, as a constant phase step per
	// pulse (1/16 turn; 6.25 Hz per unit at 100 Bd), tracked from every
	// decoded packet and control signal (RF bench: the two radios drifted
	// apart ~0.7 Hz/s).
	double afc_ = 0;
	// A new estimate b is only known modulo `mod` (data headers are QPSK:
	// a quarter turn, 25 Hz; control signals BPSK: half a turn, 50 Hz):
	// take the candidate nearest the tracked value, then smooth. Assumes the
	// offset at link start is within +-12.5 Hz and drifts < ~10 Hz per cycle.
	void afc_update(double b, double mod);
	// The peer's frequency offset as we hear it, Hz. A step bias of 1/16 turn
	// per 10 ms is 6.25 Hz: control signals and data headers both measure
	// -offset/6.25 to 1 % (exact Hilbert shifts of our own bursts, -25..+25
	// Hz, 2026-10-19). The old -0.675 x that was calibrated in links where the
	// control-signal AFC froze after its first measurement (pattern_bias_of
	// returned the prior), and since fix 64 it made the data demodulation shift
	// 1.48x the offset: the 6 m bench held a false lock (-11 Hz for +13 Hz,
	// residual 4/16 a step, hidden by the mod-4 unwrap), so the headerless
	// decode's data-only bias sat a whole symbol off. P2_AFC_OLD_SCALE: the old
	// constant (test).
	double afc_hz() const { return afc_ * kAfcHzPerUnit; }
	// A decoded data packet's bias is checked by its CRC (a level or bias a
	// quarter turn off does not decode), so it is unwrapped over the full
	// turn. Modulo a quarter turn (4/16, 25 Hz) a link starting more than
	// 12.5 Hz off locked the wrong alias from its first packet (6 m bench,
	// 2026-10-19: -10.7 Hz for +14.3 Hz until control signals pulled it back).
	// P2_AFC_DATA_MOD4: the old unwrap.
	static double afc_data_mod() { static const double m = std::getenv("P2_AFC_DATA_MOD4") ? 4.0 : 16.0; return m; }
	static inline const double kAfcHzPerUnit = std::getenv("P2_AFC_OLD_SCALE") ? 6.25 / -0.675 : -6.25;
	// Shift a burst's frequency by hz (analytic signal, Hilbert FIR).
	static void shift_audio(std::vector<float> &a, double hz, double fs)
	{
		std::vector<float> out(a.size());
		shift_audio_part(a, hz, fs, out.data(), 0);
		a.swap(out);
	}
	// shift_audio's outputs i0.. into out (the filter reaches 63 samples either side: an output more
	// than 63 samples from the end of `a` is the same in any longer window from the same start).
	static void shift_audio_part(const std::vector<float> &a, double hz, double fs, float *out, long i0);
	// The receive shift of P3 windows, kept from window to window: every window of one packet starts at
	// the same sample (slot - 0.06 s: the timing search's, the long demodulation's pieces, the decodes'),
	// so a shifted sample already made from audio that was all in is reused, and only the new ones are
	// shifted (on the Raspberry Pi 4 the long window alone took 11-15 ms of the audio loop, at the long
	// decode's launch, whenever the shift was over 3 Hz). A sample is reused only when every input it
	// read (63 either side) had been received and lies inside the new window too: the same value.
	// P3_SHIFT_CACHE_OLD=1: every window shifted whole, as before.
	struct ShiftCache { int64_t a = -1; double df = 0; long valid = 0; std::vector<float> y; };
	// x: the window from `a` (raw audio), heard: how many of its samples had been received
	static void shift_cached(std::vector<float> &x, double df, double fs, int64_t a, long heard, ShiftCache &c);
	mutable ShiftCache p3_shift_cache_;
	int64_t bk_lattice_ = -1;                        // callee break-in: the caller's packet slot (our receive timeline)
	// Fix 215: where our P2 break-in goes as callee, relative to the reply lattice `at` (hunt offset
	// included). Measured on the reference firmware (break-in at 35 s, first try, P2_TEST_BK_SHIFT_MS
	// sweep in 1 ms steps): taken from at - 2.5 to at + 0.5 ms with its own timing (rt_latency 0,
	// reply_adjust 0; Q at -3.5 and +1.5), from at - 5.5 to at - 3.5 ms with the bench's (2 ms, 1 ms;
	// Q at -6.5 and -2.5). So the caller's window for our break-in sits on the time reference of our
	// P1 answer (as for our packets after it, fix 201), and it follows our control signals slowly
	// (fix 205): 1 ms after our control-signal instant at + cs_ref_s() is its centre in both (taken
	// at +-2 ms of it with the bench's timing at -2, not at +2; the reference at 0 only). The old
	// place, at + callee_bk_adjust_s (-2.5 ms), was its early edge on the reference unit and, since fix 208
	// moved our control signals 5 ms earlier on the bench, its late edge there once the window had
	// followed them: rb13-1-2 (Q x4, T x9, nothing taken), the reference unit with the bench's timing at
	// 35 s the same (Q Q Q Q T T T T, 52 break-ins; at 25 s still taken). Fix 208's "break-ins keep
	// their old place" was the error. A repeat (nothing heard) goes in the same place (it went at
	// `at`). callee_bk_adjust_s is no longer used by the callee. P2_BK_AT_OLD=1: the old placement
	// and repeats; P2_TEST_BK_SHIFT_MS=X: our break-ins X ms later (test).
	static constexpr double kBkAfterCs = 0.001;
	double bk_rel_s() const;
	// Fix 201: as P2 callee, where our packets go after our break-in, against our reply lattice
	// (the reply instant: what we hear + 0.879 s + reply_adjust_s). The caller decodes them
	// only in a narrow window, 3-4 ms wide at level 4 and holding 0 (emulated firmware, our
	// first packet moved alone; README fix 201), and the window follows
	// our P1 answers in the set-up (kP1Reply - rt_latency_s, which the caller measures) more
	// than our P2 control signals: rt_latency_s 2 ms alone moved it 2 ms earlier. So they go
	// at the instant consistent with our P1 answers: the reply lattice - (rt_latency_s +
	// reply_adjust_s), unchanged when both are 0 (the reference unit). On the bench (2026-11-10)
	// The self-timing set rt_latency 2 ms and reply_adjust 1 ms (its two constants are
	// 3 ms apart): our packets went 3 ms late, as in rb1-2 (repeat requests, speed-downs,
	// long packets repeated). P2_BK_DATA_OLD=1: on the reply lattice (as before);
	// P2_BK_DATA_LEAD_MS=L: L ms earlier still; P2_DATA_SHIFT_MS=X (test): X ms from the
	// reply lattice instead.
	// Fix 215: 1 ms (was 0): our packets after the break-in in the break-in's own place, 1 ms after our
	// control-signal instant (fix 208 put those 2 ms before this reference, and the caller's window
	// follows them). Emulator, the firmware calls and idles, we break in with 12,000 characters at 70 s,
	// 35 s of link (P2_DATA_SHIFT_MS): 0 ms 629 characters (long packets repeated, R R R ...), -1 / -1.5 /
	// -2 ms 3,930 each (all it sends in that time); at 35 s 3,930 for 0, -1 and -2. With the bench's
	// timing at 35 s: -3 ms (lead 0) 2,902, -4 ms (lead 1) 3,930.
	static constexpr double kBkDataLead = 0.001;
	int64_t bk_data_shift();
	// When to look at the answer to our break-in at `at`. The next break-in
	// (if any) goes out 1.25 s after this one, so the decision must come
	// before that: a check at +1.271 s (0.971 + 0.30) started every repeated
	// break-in 21 ms late, its first pulses cut off (RF bench). The caller's
	// answer (0.24 s) is in by its lattice + 1.85 + 0.25 s.
	int64_t bk_check_at(int64_t at) const
	{
		if (!master_ && !p3_ && bk_lattice_ >= 0) return std::min(bk_lattice_ + samples(cyc_s() + 0.6 + 0.25), at + reply_delay_theirs() + samples(0.30));
		return at + reply_delay_theirs() + samples(0.30);
	}
	// When to look at the answer to our data packet at `ref` (lng: a long
	// packet). The same limit as bk_check_at: our next packet goes out at the
	// next slot (1.25 s, long 3.75 s), so the look must come before it. As
	// callee sending data the usual look (0.971 + 0.30 = 1.271 s) was 21 ms
	// after that slot: every packet after the first one of a turn left with
	// its first two pulses (the header) cut off, and the caller decoded none
	// of them (P2 after the callee's break-in, 2026-10-19: loopback, LinBPQ
	// and 6 m). 5 ms before the slot reply_rx classifies at once, so the
	// packet is queued in time however long that takes; the control signal
	// (0.25 s from +0.971 s) is in by then. P2_LATE_REPLY_LOOK: the old look.
	// Fix 253, P3: the same for the callee sending data. Its look (0.971 + 0.30 = 1.271 s, long
	// 3.771 s) came after its next packet's leading tail (1.238 s): every packet it sent after a
	// look was queued ~33 ms after its start (its 20 ms lead and the first ~10 ms of the packet
	// never sent), with the reference unit as well as between two of our stations. It now comes
	// just before that leading tail (one cycle less the leading tail and 3 ms: 1.235 s, long
	// 3.735 s, as fix 252's callee_look_at and fix 250's look deadline); the caller's control
	// signal arrives at +0.965 s. The caller's look (0.879 + 0.30 s) is unchanged.
	// P3_DATA_LOOK_OLD=1: the old look.
	// P2 (TODO Now d, 2026-11-15): the callee's cap at slot - 5 ms still came after its next
	// packet's 12 ms leading tail (slot - 12 ms): every packet it sent after a look was queued
	// ~7 ms after its start (7 of the first pulse's 12 ms leading tail never sent; regression
	// p2-tx-ppm130 31 of 31, compat p2-it-call-we-send). It now comes where fix 253 puts P3's,
	// just before that leading tail (p2_look_deadline: slot - 15 ms, 1.235 s, long 3.735 s), and
	// is classified at once there (reply_rx's deadline the same); the caller's control signal
	// arrives at +0.971 s and lasts ~0.25 s. The caller's look (0.879 + 0.30 s) is unchanged.
	// P2_IRS_LOOK_OLD=1: the old cap (slot - 5 ms).
	static bool p2_irs_look_old() { static const bool old = std::getenv("P2_IRS_LOOK_OLD") != nullptr; return old; }
	int64_t p2_look_deadline(int64_t next) const { return p2_irs_look_old() ? next - samples(0.003) : p3_look_deadline(next); }
	int64_t reply_check_at(int64_t ref, bool lng) const;
	static double wrap16(double b) { b = std::fmod(b + 8.0, 16.0); if (b < 0) b += 16.0; return b - 8.0; }
	// Is the burst starting near `exp` a break-in packet (its header)?
	// Fix 250: the reading (bk_look, const, from a snapshot on a worker in real time) and what it
	// changes (looks_like_bk_apply) are apart; looks_like_bk does both at once, as before.
	struct BkLook { int hit = 0; double start = 0, bias = 0, soft = 0, c[2] = {0, 0}; double own_soft = -1, own_moved = 0, own_rej = -2; int own_slip = 0; double own_lag = 0; double own_hard = -999; };   // hit: 1 at the tracked timing (soft), 2 hard;
	                                                                 // own_*: fix 266 (the burst's own timing took over a weak tracked reading)
	BkLook bk_look(int64_t exp, const CsPred &pred, const Snap *sp, double afc, int64_t heard) const;
	// The break-in's pattern matched hard on the burst's own demodulation (untracked).
	bool bk_hard(const Demod &d0, int64_t exp, const Snap *sp, double afc, int64_t heard, double &start, double *bias) const;
	bool looks_like_bk(int64_t exp, double &start, bool track = false)
	{
		CsPred pred;
		if (track && !std::getenv("P2_NO_BKTRACK")) pred = cs_pred(exp);
		return looks_like_bk_apply(exp, pred, bk_look(exp, pred, nullptr, afc_, n_), start);
	}
	bool looks_like_bk_apply(int64_t exp, const CsPred &pred, const BkLook &b, double &start);
	int64_t bk_off_exp_ = -1; double bk_off_ = 0;     // fix 74: the last off-track hard break-in
	bool bk_match(const Demod &d, double &start) const { return bk_match(d, start, afc_, n_); }
	bool bk_match(const Demod &d, double &start, double afc, int64_t heard, double *bias_out = nullptr) const;
	struct RxPacket {
		bool ok = false, bk = false, lng = false; int level = 0; std::vector<uint8_t> info; double start = 0, bias = 0;
		// a failed short packet with a header: its soft values, for memory ARQ
		std::vector<float> llr; int llr_level = 0, llr_parity = -1; double llr_rms = 99;
		int swap = -1;                                   // carrier order of the candidate
		int metric = -1, marq_n = 0; double hdr_rms = 99;   // decode diagnostics (Viterbi metric, MARQ copies, header fit)
		double c[2] = {0, 0}; bool has_c = false;         // the carrier timing it decoded with (timing tracking)
		double rc[2] = {0, 0}; bool has_rc = false;       // that timing refined from the decoded data (fix 212; worker)
		double bkc[2] = {0, 0}; bool has_bkc = false;     // a break-in: the carrier timing it decoded with (its quality, item 13)
		// memory ARQ decoded ahead on the worker (P2, real time): the sum of the
		// stored copies (version mq_ver) plus this one, decoded
		bool mq_pre = false, mq_pre_ok = false; uint64_t mq_ver = 0; std::vector<uint8_t> mq_info; int mq_metric = -1;
		int path = 0, pred = -1;                         // debug: which path decoded it (kPathName), the predicted carrier order
	};
	static constexpr const char *kPathName[8] = {"-", "tracked", "tracked-search", "tracked-headerless", "acquisition", "own-timing", "slipped", "headerless"};
	// P3 speed level 1: descramble a decoded P2 level-1 info block in place
	// (-> 5 data bytes + plain status); false if its CRC fails.
	static bool sl1_plain(std::vector<uint8_t> &info)
	{
		if (info.size() < 8) return false;
		uint8_t d[p3::kSl1Data], st;
		if (!p3::sl1_check(info.data(), d, st)) return false;
		for (int i = 0; i < p3::kSl1Data; i++) info[size_t(i)] = d[i];
		info[p3::kSl1Data] = st;
		return true;
	}
	// Decode a data packet (short/long, or break-in) starting near `exp`.
	// Fix 64: P2 data packets are fitted at the AFC's tones (the joint pulse
	// fit at the nominal tones leaked between the carriers, 200 Hz apart, at a
	// 20 Hz offset: P2 L1 decoded 76-81 % against 94 % at 0 Hz, -8.9 dB); the
	// measured bias is then the residual, returned as absolute (+ afc_) so the
	// AFC loop and everything else keep their frame. P2_NO_DEMOD_AFC: off.
	// The timing track as one look sees it (fix 200): taken on the audio loop when a decode is
	// launched on a worker (trk_view), so the worker reads no tracking state.
	struct TrkView { bool on = false; int swap = -1; double dt = 0, lead = 0, stag = 0; };
	// Fix 213: P3 SL1 data packets (and P3 break-ins read here) at the AFC's tones too. P3's levels
	// 2-6 move their audio by p3_rx_df_ (fix 49), but SL1 came through here at the nominal tones:
	// sl1_sens P3 SL1 at +-20 Hz decoded ~7-10 points below 0 Hz. P3_NO_DEMOD_AFC: as before.
	bool demod_afc_on() const { static const bool off3 = std::getenv("P3_NO_DEMOD_AFC") != nullptr; return !p3_ || !off3; }
	// the tones' shift find_packet demodulates at (the AFC)
	double packet_fshift(bool bk_only) const;
	// the tracked timing of both carriers (find_packet_at's first try), cc: the early timing if any
	void tracked_c2(int64_t exp, const TrkView &tk, const double *cc, double c2[2]) const
	{
		const double want_start = double(exp) + tk.dt;
		const int sw = tk.swap;
		const double lead = want_start + tk.lead, lag = lead + tk.stag;
		c2[0] = sw ? lag : lead; c2[1] = sw ? lead : lag;
		trk_retime(cc, c2);
	}
	RxPacket find_packet(int64_t exp, bool bk_only, bool allow_long, const Snap *sp = nullptr, const double *cc = nullptr, const TrkView *tv = nullptr);
	RxPacket find_packet_at(int64_t exp, bool bk_only, bool allow_long, const Snap *sp, const double *cc, double fshift, const TrkView *tv);
	// ---- memory ARQ ----------------------------------------------------------
	// The receiver adds the soft values of a packet's failed copies (same level
	// and header parity: the sender repeats a packet we did not acknowledge)
	// and decodes the sum (the 1990 protocol description; "MARQ"). Each copy
	// is weighted by its header fit (1/rms^2). Cleared by any good packet, a
	// new level or parity, or after kMarqMax copies.
	static bool wsoft_on() { static const bool on = std::getenv("P2_NO_WSOFT") == nullptr; return on; }   // fix 105: weighted soft decoding of P2 data packets
	static bool marq_on() { static const bool on = std::getenv("P2_NO_MARQ") == nullptr; return on; }   // test: P2_NO_MARQ turns it off
	static constexpr int kMarqRestart = 6;
	std::vector<float> marq_sum_;
	int marq_level_ = 0, marq_parity_ = -1, marq_n_ = 0, marq_swap_ = -1;
	uint64_t marq_ver_ = 0;                           // changes with every change of the sum (the worker's snapshot is still valid when equal)
	void marq_clear() { marq_sum_.clear(); marq_n_ = 0; led_marq_ = 0; marq_ver_++; }
	// The memory-ARQ trial as marq() would make it, on a snapshot (worker).
	static float marq_weight(const RxPacket &q) { return float(1.0 / std::max(0.25, q.llr_rms * q.llr_rms)); }
	// A sum of a long packet's soft values (40 pulses a group) or a short one's (9).
	static int llr_ppr(int level, size_t n) { return n == size_t(2 * (p2::kInfoBits(level, 40) + 8)) ? 40 : 9; }
	struct MarqSnap { std::vector<float> sum; int level = 0, n = 0; uint64_t ver = 0; };
	static void marq_ahead(RxPacket &q, const MarqSnap &m);
	// ---- timing tracking ----------------------------------------------------
	// A packet's carrier timing from its own first pulses (timing_of) fails
	// at low SNR long before the decoder does: with the true timing SL1 / P2
	// level 1 decode ~99 % of first copies where per-packet timing gets
	// 14 % (a bench tool, -8.6 dB in 3 kHz), and a timing error over
	// ~1.5 ms is already costly. In a link the peer's packets keep their
	// timing against ours, so every good decode gives the next one: its
	// start against the expected one (trk_dt_), the leading carrier's first
	// pulse against the start (trk_lead_), the carriers' stagger, and the
	// carrier order (the leading carrier alternates with each transmission:
	// learned from consecutive good packets, trk_alt_). find_packet tries the
	// tracked timing first. P2_NO_TRACK turns it off (test).
	bool trk_ok_ = false, trk_alt_ = true;
	double trk_dt_ = 0, trk_lead_ = 0, trk_stag_ = 0;
	int trk_swap_ = 0;
	// Fix 200: the peer's transmissions numbered by slot. Each slot looked at gets the next number
	// (trk_view, on the audio loop); the track remembers the number of the slot that taught it
	// (trk_seq_), and the carrier order predicted for a slot follows from the difference. It was a
	// count of looks since the last update (trk_calls_), kept on the decode workers and zeroed by
	// each update: a late decode (fix 149) teaches the track only after the next slot was counted
	// (its decode is launched at 0.845 s, the late result taken at the look, 0.86 / 3.36 s), so that
	// slot's count was lost and every later prediction stayed one transmission behind while the
	// decodes stayed late (each then took the slow paths: once late, always late).
	int64_t trk_n_ = 0, trk_seq_ = 0;
	std::array<std::pair<int64_t, int64_t>, 16> trk_slots_{};   // (slot, number); number 0: empty
	size_t trk_slot_i_ = 0;
	static bool track_on() { static const bool on = std::getenv("P2_NO_TRACK") == nullptr; return on; }
	void trk_clear() { trk_ok_ = false; trk_alt_ = true; trk_seq_ = trk_n_; for (auto &e : trk_slots_) e = {0, 0}; trk_v_ = 0; trk_vacc_ = 0; trk_v_exp_ = -1; trk_v_n_ = 0; trk_v_i_ = 0; }
	// The number of the slot starting at `exp` (add: a new one if it has none; else 0).
	int64_t trk_seq_of(int64_t exp, bool add)
	{
		for (const auto &e : trk_slots_) if (e.second > 0 && e.first == exp) return e.second;
		if (!add) return 0;
		trk_slots_[trk_slot_i_] = {exp, ++trk_n_};
		trk_slot_i_ = (trk_slot_i_ + 1) % trk_slots_.size();
		return trk_n_;
	}
	// The track for the transmission at `exp` (audio loop; counts the slot). count = false: as it
	// would be, without counting (a decode that may not need it; the caller counts later).
	TrkView trk_view(int64_t exp, bool count = true)
	{
		TrkView v;
		int64_t q = trk_seq_of(exp, count);
		if (q <= 0) q = trk_n_ + 1;
		v.on = trk_ok_ && track_on();
		v.swap = trk_swap_ ^ (trk_alt_ ? int((q - trk_seq_) & 1) : 0);
		v.dt = trk_dt_; v.lead = trk_lead_; v.stag = trk_stag_;
		return v;
	}
	// Fix 197: the tracked look re-timed from the packet's own early timing (the clean tree's
	// P2_TRK_RETIME, its PROVENANCE H3/H4/E58/H28, ported in its default form). A packet that
	// decodes at the tracked timing gives that timing back unchanged, so the track never learns
	// from its own decodes: our next look (and as callee our control signal after it) kept an
	// exact cycle of our clock while the peer's walked, until a decode failed and the timing was
	// measured again (a sawtooth). The early timing of the same packet (cc: timing_of over its
	// first 0.3 s, computed for every look anyway) moves the prediction by half the offset when
	// both carriers agree with it within 0.3 pulse and with each other within 0.15 pulse (a pulse
	// slip, the reason for tracking, is not taken). On the bench it took the clean side's P2
	// receive from 1,780-2,404 to 3,000 x4 against a reference unit 130 ppm off [bb-clean E58].
	// P2_TRK_RETIME=0: off; =1: the full offset.
	static double trk_retime_gain() { static const double g = std::getenv("P2_TRK_RETIME") ? (std::atoi(std::getenv("P2_TRK_RETIME")) == 0 ? 0.0 : std::atoi(std::getenv("P2_TRK_RETIME")) == 1 ? 1.0 : 0.5) : 0.5; return g; }
	void trk_retime(const double *cc, double *c2) const;
	// Fix 212: the decoded packet's own timing refined from its data, for the timing track. A
	// decode at the tracked timing returned that timing, so the track learned only from packets
	// that failed at it and decoded at their own timing (timing_of, ~1 ms off at -10 dB); fix 197
	// re-times the look from the early timing, this refines what the track learns. The known
	// data steps (re-encoded from the decoded block) are correlated against the pulses at the
	// timing -h, 0, +h on each carrier (matched filter per pulse); a parabola through the three
	// gives each carrier's error. Short packets, P2 levels 1-4 and P3 SL1. Computed on the decode
	// worker when it has a snapshot (find_packet, has_rc), else in trk_update; ~0.43 ms on the
	// build VM. P2_NO_TRK_REFINE: off.
	bool trk_refine(const RxPacket &q, double c[2], const Snap *sp = nullptr) const;
	void trk_update(const RxPacket &q_in, int64_t exp);
	// Fix 247: the track's dt is the peer's start against the slot it was taught on (exp). As P2 IRS
	// the next slot is counted from the start we decoded (reply_as_irs(p.start): next = p.start + a
	// cycle), which already holds the tracked dt (a decode at the tracked timing returns exp + dt, plus
	// fix 197's re-time); the next look then added the same dt again. So every cycle our slots moved by
	// the whole tracked dt: a lattice integrating the low-passed error, z^2 - 1.4z + 0.7, poles 0.84 at
	// +-33 deg, and with fix 212's refinement clamped at +-h (an error beyond 1.25 ms reads as 1.25 ms)
	// a swing larger than the error it started from. p2-snr-3-it-call-it-send (the reference unit
	// calls and sends, -3 dB): our first P2 slot 2 ms late of its packets (our call timing in -3 dB), and the
	// slots then swung to 3.3 ms early within 5 cycles (and back, 4 cycles later), so our control signals
	// left the window our P1 answer had placed (three more read, then none); its
	// window then walks away from us and the link was dead from 22 s. Now the
	// shift the slot took is taken off the track (the same absolute prediction in the new slot's frame):
	// the slots follow the track's estimate of the peer's start. That alone is a first-order loop: against
	// a clock skew its slots lag and catch up only at good decodes (between them they keep our exact
	// cycle), and autocal's lattice reading (fix 238, from our transmissions) saw those flat stretches:
	// the autocal P2 200 ppm test read -162.6 ppm for the true -200 (window -235..-165). So the slots also
	// carry the peer's drift a cycle (trk_v_, samples per short cycle, every slot, good or not), the
	// slope of the track's estimates of the peer's start (each good decode's exp + dt, against its slot
	// number) over the last kTrkVPts good decodes, Theil-Sen (the median of the pairwise slopes: the
	// acquisition's first moves and a re-taken track are outliers to it), once kTrkVMin of them span
	// kTrkVSpan cycles; at most +-kTrkVMax (400 ppm). (A drift learnt from each move, a second-order
	// loop, took the acquisition's 12-sample move for a drift of -0.9 samples a cycle and walked our
	// answers off the window: p2-snr-3-it-call-it-send 689.) P2_TRK_FRAME_OLD=1: as before (P3
	// unchanged: its slots are p3_slot()'s).
	static constexpr int kTrkVPts = 16, kTrkVMin = 8, kTrkVSpan = 12;
	static constexpr double kTrkVMax = 4.8;
	double trk_v_ = 0, trk_vacc_ = 0;                // fix 247: the peer's drift (samples a short cycle); its fraction carried
	int64_t trk_v_exp_ = -1;                         // the slot numbering's origin (fix 247)
	double trk_v_k_[kTrkVPts] = {}, trk_v_y_[kTrkVPts] = {};
	int trk_v_n_ = 0, trk_v_i_ = 0;
	static bool trk_frame_old() { static const bool v = std::getenv("P2_TRK_FRAME_OLD") != nullptr; return v; }
	void trk_reframe(int64_t moved, int64_t exp);
	// fix 247: the drift the next slot carries, `cycles` short cycles on (whole samples; the fraction kept)
	int64_t trk_drift_step(double cycles)
	{
		if (p3_ || trk_frame_old() || trk_v_ == 0) return 0;
		trk_vacc_ += trk_v_ * cycles;
		const int64_t k = int64_t(std::llround(trk_vacc_));
		trk_vacc_ -= double(k);
		return k;
	}
	RxPacket marq(RxPacket q, int64_t exp);

	// ---- sending data ----------------------------------------------------
	static bool long_known(int level) { return p2arq::kLongHeader[level][0][0] != 0; }
	bool long_ok(int level) const { return cfg_.allow_long && long_known(level); }
	// Fix 217: the status's 0x20 bit. P2: more text follows this packet. P3: the reference unit as
	// sender sets a "much to send" flag in the status while it holds plenty of text to send, and as
	// receiver asks for long packets (R) on
	// it, and for short ones again when it clears. On a short packet at
	// SL2-SL6 our codec carries the status XOR 0x22 against the reference unit's (its reading of
	// our packets' status: ours ^ 0x22, 2026-11-11), so there the
	// flag is our 0x20 CLEAR; on a long packet it is our 0x20 set (no XOR). We always set 0x20:
	// to the reference unit our short packets said "little to send" and it never asked R (P3 we send,
	// clean: 4,162 against the reference-unit pair's 6,788). SL1 is left as it was (not measured).
	// P3_MORE_OLD=1: always 0x20 as before.
	// Fix 229: NOT at SL4. Watched on the reference unit (p3-moderate-we-call-we-send and p3-we-call-we-send,
	// its received status against what we sent): SL3, SL5, SL6 short ours ^ 0x22, but SL4 short ours ^ 0x02
	// (0x10 -> 0x12, 0x11 -> 0x13, 0x1A -> 0x18 on 18 packets). So at SL4 our "much to send" was read
	// inverted both ways: the reference unit never asked S or R from our SL4 packets, and our R exception at SL4
	// covered the same thing on receive. P3_SL4_XOR_OLD=1: the old rule.
	bool p3_short_xor(int level, bool lng) const
	{
		static const bool sl4_old = std::getenv("P3_SL4_XOR_OLD") != nullptr;
		return !lng && level >= 2 && (level != 4 || sl4_old);
	}
	// Fix 244: the flag on our queue alone, >= 5 packets' worth (5 x the data bytes of a short packet,
	// 5/3 x those of a long one), raised on the
	// first text packet after idle too (checked before each new packet: no
	// other condition). It used to be held below SL6 until a packet after a full one, which
	// matched the reference-unit pair only where the whole text was handed over at once (the
	// pair's host typing 1,000 characters every 2 s keeps under 5 SL5 packets' worth queued at
	// the first text packet). Where the pair has more queued than that (CCIR moderate, SL3: its
	// first text packet after idle carries the flag, status 0x30) the reference unit asks R
	// (long SL3); held, ours got no R, then S, and long SL5 failed twice in the fading (2,079
	// characters against the pair's 2,421). P3_MORE_HOLD=1: the old hold.
	uint8_t more_bit(bool more, size_t queued, int cap) const;
	// The firmware's "much to send" flag in a P3 status we received (level 1: our 0x20, as before)
	bool p3_rx_lots(uint8_t status, int level, bool lng) const
	{
		static const bool old = std::getenv("P3_MORE_OLD") != nullptr;
		const bool b = (status & 0x20) != 0;
		return old ? b : b != p3_short_xor(level, lng);
	}
	int capacity() const { return p3_ ? (p3_long_ ? p3::long_payload(p3_level_) : p3::level_data_bytes(p3_level_)) : p2::data_bytes(level_, long_ ? 40 : 9); }
	void build_packet(bool new_info);
	// Fill pending_data_ (cap bytes) from the queue with the data type that
	// carries the most text (ASCII / Huffman / swapped / PMC), as the reference unit does.
	int fill_pending(int cap);
	// Re-pack the same information for a new level/length (same counter).
	void rebuild_same()
	{
		const int cap = capacity();
		const int type = fill_pending(cap);
		const bool more = txq_.size() > size_t(pending_used_);
		pending_status_ = uint8_t((pending_status_ & ~0x3c) | type << 2 | more_bit(more, txq_.size(), cap));   // as build_packet (fix 217)
		transmit_pending();
	}
	// The words of one data packet in the current mode, and how they are
	// modulated: a pure function of (data, status, counter, first), used for
	// the packet sent now and for the ones made ahead (premod_next).
	enum TxKind { TX_PSK, TX_P3, TX_LV, TX_CS };   // TX_CS: P3's PSK bursts (SL1)
	struct TxWords { TxKind kind; int level; bool lng; std::vector<uint8_t> w; };
	// bytes of data in a packet of the current mode
	int tx_payload() const;
	TxWords tx_words(const std::vector<uint8_t> &data, uint8_t st, int counter, bool first) const;
	void transmit_pending();
	// ---- transmit audio made ahead -------------------------------------
	// Our next packet's words are known only once the peer's control signal
	// says new or repeat (or a level change), and its modulation (13 ms on
	// tardis, 26 ms on a starved worker) then had to finish before the packet
	// was due: late at normal worker priority under load (2026-10-18). Now,
	// as soon as a packet is queued, the two likely next ones are modulated on
	// a worker: the repeat, and the next new packet (a dry run of
	// build_packet). When the real next packet is queued, audio made ahead is
	// used only if every input of the modulation matches exactly (words,
	// carrier map, level, length, gain, shift); otherwise it is made then, as
	// before. A wrong guess costs CPU, never the signal. P3_NO_PREMOD=1: off.
	struct PremodKey {
		TxKind kind; int level; bool lng; bool sw; float amp; double shift; std::vector<uint8_t> w; double lead = 0.0; float clip = 0.0f;
		bool operator==(const PremodKey &o) const { return kind == o.kind && level == o.level && lng == o.lng && sw == o.sw && amp == o.amp && shift == o.shift && lead == o.lead && clip == o.clip && w == o.w; }
	};
	struct Premod { PremodKey key; std::shared_future<std::vector<float>> fut; std::shared_ptr<TxTrace> tr; std::shared_ptr<TxProg> prog; const char *why = nullptr; };   // why: fix 273's guesses
	// a P2 long packet without a static shift: made in pieces (TxProg)
	bool pieces_key(const PremodKey &k) const { return tx_pieces_on() && cfg_.realtime && pool_.size() > 0 && k.kind == TX_PSK && k.shift == 0.0; }
	std::shared_ptr<TxProg> prog_new(const PremodKey &k) const
	{
		auto pr = std::make_shared<TxProg>();
		pr->plan = p2::modulate_plan(k.w, cfg_.fs, cfg_.tone_lo, cfg_.tone_hi, k.sw, k.amp, 0.0f);
		pr->y.assign(pr->plan.len, 0.0f);
		return pr;
	}
	void submit_prog(const std::shared_ptr<TxProg> &pr, const std::shared_ptr<TxTrace> &tr, bool urgent)
	{
		pool_.submit(traced(tr, [pr]() { pr->make(); return 0; }), urgent);
	}
	std::vector<Premod> premod_;
	bool premod_hit_ = false, premod_stale_ = false;
	const char *premod_why_ = nullptr;                 // the packet just queued: which of fix 273's guesses made it (nullptr: none)
	int premod_hits_ = 0, premod_misses_ = 0;
	bool premod_on() const { static const bool off = std::getenv("P3_NO_PREMOD") != nullptr; return !off; }
	// P3_PREMOD_CHECK=1 (simulation): the guesses are compared with the packets
	// sent, nothing is made ahead (the hit count tests predict_new offline)
	static bool premod_check() { static const bool on = std::getenv("P3_PREMOD_CHECK") != nullptr; return on; }
	// The audio made ahead for this burst, if any (and counts hit or miss).
	std::shared_future<std::vector<float>> premod_take(const PremodKey &k, std::shared_ptr<TxTrace> *tr = nullptr, std::shared_ptr<TxProg> *prog = nullptr);
	// a level in dB from the environment; unset or empty: the default
	static double env_db(const char *name, double def) { const char *e = std::getenv(name); return e && *e ? std::atof(e) : def; }
	// P2 PSK bursts (packets, control signals): P2_GAIN_DB raises them over
	// the P1 level (2026-10-18: at one amplitude our P2 data peaks ~1 dB under
	// our P1 peak and ~3 dB under our P3 peaks; the reference firmware sends
	// P2 3.7 dB over its P1 RMS). Default +2 dB.
	// P3's two-carrier PSK bursts (control signals, SL1 data, set-up, break-ins):
	// P3_PSK_GAIN_DB over kP3CsGain (2026-10-18: our CS RMS 1,208 / peak 2,467
	// at amplitude 2970, the reference firmware's 1,804 / ~3,600; ours 1.5 dB
	// under our peak-reduced data, the firmware's 2.5 dB over its own). Default
	// +3 dB. Both checked on the air in the night soak of 2026-10-18/19: ALC 0
	// in every session, 37-47 W under the 0128 limit; the raised peaks stay
	// under our P3 data peaks. 0 = the old levels.
	float p3_psk_amp() const { static const float g = std::pow(10.0f, float(env_db("P3_PSK_GAIN_DB", 3.0)) / 20.0f); return kP3CsGain * cfg_.amplitude * g; }
	float p2_amp() const { static const float g = std::pow(10.0f, float(env_db("P2_GAIN_DB", 2.0)) / 20.0f); return cfg_.amplitude * g; }
	// P3 data bursts: the modulators, then clip-and-filter peak reduction
	// (p3_papr.hpp): clip at 2 x RMS, the peak kept, so at the same peak
	// (a transmitter's limit) ~3.7 dB more average power. Emulator, noise on
	// what the firmware hears, our text to it: 2.2-2.7x the characters
	// (README). P3_PAPR=R: another clip level, 0 = off.
	static float p3_papr_clip() { static const float v = std::getenv("P3_PAPR") ? float(std::atof(std::getenv("P3_PAPR"))) : 2.0f; return v; }
	// Fix 217: SL5 short packets on a clean link are clipped at P3_PAPR_CLEAN (3 x RMS) instead:
	// the reference unit as receiver asks S to SL6 only on SL5 packets it reads cleanly enough
	// (judged for each packet), and with the
	// 2 x clip our text packets often fell short of that where its own pair's did not (2026-11-11):
	// no S, R at SL5 instead, long SL5 packets before SL6. At 3 x the S comes (the
	// reference-unit pair's own sequence). Clean = our last kP3CleanRun packets taken at the first try;
	// any repeat request, T or missed answer goes back to 2 x (noise: the power matters there;
	// 3 x everywhere cost p3-send-noise 5,285 -> 4,582, p3-send-fade 2,656 -> 1,850).
	static constexpr int kP3CleanRun = 4;
	float p3_clip_now() const
	{
		static const float clean = std::getenv("P3_PAPR_CLEAN") ? float(std::atof(std::getenv("P3_PAPR_CLEAN"))) : 3.0f;
		const float base = p3_papr_clip();
		return base > 0 && clean > 0 && p3_clean_run_ >= kP3CleanRun ? clean : base;
	}
	int p3_clean_run_ = 0;                            // fix 217: our packets taken at the first try in a row
	static constexpr double kP3LongMaxDfTx = 5.0;     // fix 217b: no "much to send" flag beyond this offset
	// test hook (fix 273): P3_TEST_TX_US=U, a P3 data burst made as slowly as
	// on a slow core: U microseconds more per 256 samples (600: ~20 ms a short packet, ~75 ms a long one)
	static void p3_test_tx_slow(size_t n)
	{
		static const int us = std::getenv("P3_TEST_TX_US") ? std::atoi(std::getenv("P3_TEST_TX_US")) : 0;
		if (us > 0) std::this_thread::sleep_for(std::chrono::microseconds(int64_t(us * double(n) / 256.0)));
	}
	static std::vector<float> mod_p3(const std::vector<uint8_t> &w, float fs, bool sw, float amp, double lead, float clip = -1.0f)
	{
		auto y = p3::modulate(w, fs, sw, amp, lead);
		p3_test_tx_slow(y.size());
		if (clip < 0) clip = p3_papr_clip();
		return clip > 0 ? p3::papr_reduce(y, fs, clip, 2) : y;
	}
	static std::vector<float> mod_lv(int level, const std::vector<uint8_t> &w, float fs, bool sw, float amp, double lead, bool lng)
	{
		auto y = p3::modulate_lv(level, w, fs, sw, amp, lead, lng);
		p3_test_tx_slow(y.size());
		return p3_papr_clip() > 0 ? p3::papr_reduce(y, fs, p3_papr_clip(), 2) : y;
	}
	static std::vector<float> premod_make(const PremodKey &k, float fs, float tone_lo, float tone_hi);
	// The key queue_* would use for these words now (false: not a burst made
	// on a worker, nothing to make ahead).
	bool premod_key(const TxWords &t, PremodKey &k) const;
	// the key of a PSK burst queued now (queue_psk)
	PremodKey psk_key(const std::vector<uint8_t> &w, double shift) const
	{
		if (p3_) return PremodKey{TX_CS, 1, false, psk_swap_, p3_psk_amp(), 0.0, w};
		return PremodKey{TX_PSK, level_, long_, psk_swap_, p2_amp(), shift, w};
	}
	// The next new packet's data and status, as build_packet(true) would make
	// them now, without changing anything.
	bool predict_new(std::vector<uint8_t> &d, uint8_t &st) const;
	// (Called again when new text arrives while we send: audio already made
	// for an unchanged guess is kept.)
	// fresh: a packet was just queued (else: text arrived while we send, the guesses made again)
	void premod_next(bool fresh = true);
	static constexpr double kChgRedoS = 0.5;
	int64_t chg_at_ = -1000000000;                    // fix 273: when the change guesses were last made
	bool chg_redo_ = false;                           // fix 273: text came since: make them again at chg_at_ + kChgRedoS
	// Fix 273: P3 packets whose words change with the receiver's answer, made ahead too. A level change
	// (T, P or a failed speed-up: the same information again one level lower; S: the next packet one level
	// higher) and R (the next packet long, or short again) gave a packet that premod_next did not guess:
	// it was modulated and peak-reduced whole (papr_reduce over the whole burst: it cannot be made in
	// pieces as P2's are) only after the answer, and on a slow or one-core computer the audio loop waited
	// for it at its start ("TX audio not ready", Pi 4: 4-60 ms, ~3 a run as sender). Its words are a pure
	// function of the queue and of the state the answer sets, so as soon as a packet is queued these are
	// modulated on a worker too, after the two guesses above: one level lower (the repeat as rebuild_same
	// makes it), one level higher (the next packet as build_packet makes it after S), the other packet
	// length (after R); and at SL5 the repeat and the next packet with the clip (fix 217) that a repeat
	// request or the acknowledgement then sets. As before, audio made ahead is used only when every input
	// matches the packet queued (premod_take), so the audio is the same numbers and a wrong guess costs CPU
	// (the sender's worker is otherwise idle while its packet is on the air), never the signal.
	// P3_TX_EARLY_OLD=1: not made ahead (the old way).
	static bool p3_tx_early_old() { static const bool v = std::getenv("P3_TX_EARLY_OLD") != nullptr; return v; }
	// The repeat as rebuild_same would make it now (at the current level and length), without changing anything.
	bool predict_same(std::vector<uint8_t> &d, uint8_t &st) const;
	template <class Add> void premod_p3_changes(Add &add)
	{
		if (qrt_end_sent_ || pending_qrt_) return;
		const int lv0 = p3_level_, cr0 = p3_clean_run_, hf0 = hdr_flip_;
		const bool lng0 = p3_long_;
		auto restore = [&]() { p3_level_ = lv0; p3_long_ = lng0; p3_clean_run_ = cr0; hdr_flip_ = hf0; };
		std::vector<uint8_t> d; uint8_t st = 0;
		// SL5 short packets: the clip follows the clean run (fix 217), which a repeat request zeroes and
		// an acknowledgement counts up before the packet is made
		if (lv0 == 5 && !lng0) {
			p3_clean_run_ = 0;
			add(tx_words(pending_data_, pending_status_, counter_, false), "repeat");
			p3_clean_run_ = cr0 + 1;
			if (predict_new(d, st)) add(tx_words(d, st, (counter_ + 1) & 3, false), "next packet");
			restore();
		}
		if (lv0 > 1) {                                  // T, P, a failed speed-up, R repeated: down one level, the same information
			p3_level_ = lv0 - 1; p3_clean_run_ = 0; hdr_flip_ = counter_ & 1;
			if (predict_same(d, st)) {
				const TxWords t = tx_words(d, st, counter_, false);
				add(t, "level down");
				// P after the break-in's answer keeps the clean run: to SL5 short with the clean clip
				if (p3_level_ == 5 && !lng0 && cr0 >= kP3CleanRun) { p3_clean_run_ = cr0; add(t, "level down"); }
			}
			restore();
		}
		if (lv0 < 6) {                                  // S: up one level, the next packet
			p3_level_ = lv0 + 1; p3_clean_run_ = cr0 + 1;
			if (predict_new(d, st)) add(tx_words(d, st, (counter_ + 1) & 3, false), "level up");
			restore();
		}
		if (!std::getenv("P3_NO_LONG") && (lng0 || p3::long_payload(lv0) > 0)) {   // R: the other packet length, the next packet
			p3_long_ = !lng0 && p3::long_payload(lv0) > 0; p3_clean_run_ = cr0 + 1;
			if (predict_new(d, st)) add(tx_words(d, st, (counter_ + 1) & 3, false), p3_long_ ? "long packets" : "short packets");
			restore();
		}
	}
	// Fix 254: our first packet after our changeover break-in, made ahead. Its words are known only
	// once the break-in's answer says the level (T: SL5, S: SL3, P: SL1), read just before that
	// packet is due (fix 252: one cycle less its leading tail and 3 ms), so its audio was made then
	// and the audio loop waited for it at its start: on a Raspberry Pi 4 8-20 ms ("TX audio not
	// ready"), a playback underrun. Now, as our break-in is queued, the first packet for each of
	// those letters is modulated on a worker (predict_new under the state case 20 sets); the one
	// queued is taken only if every input matches (premod_take), so a wrong guess costs CPU, never
	// the signal. R (SL1 long) is not guessed. Real time only. P3_BK_PREMOD_OLD=1: not made ahead.
	void premod_after_bk();
	// After the QRT packet is acknowledged: a packet with the next counter's
	// header and an alternating 1/0 DBPSK body; its acknowledgement ends the link.
	std::vector<uint8_t> qrt_end_words(int counter) const
	{
		std::vector<uint8_t> w(p2::kHeader[level_][counter & 1], p2::kHeader[level_][counter & 1] + 9);
		for (int k = 0; k < 72; k++) w.push_back(k % 2 ? 0x22 : 0xaa);
		w.push_back(0xa2);
		return w;
	}
	static constexpr int kQrtEndSilent = 4;           // fix 76: silent looks after our end packet before we end the link
	void send_qrt_end()
	{
		misses_ = 0;
		counter_ = (counter_ + 1) & 3;
		queue_psk(next_start_, qrt_end_words(counter_));
		my_start_ = next_start_;
		qrt_end_sent_ = true;
		long_ = false;
		schedule(reply_check_at(my_start_, false), EV_REPLY, my_start_, 0);
		log("QRT end packet");
	}
	void send_bk(int64_t start);

	// ============================ P3 ===================================
	// Measured on the reference firmware (the protocol description section 4.1): the callee
	// answers 0.8848 s after the slot it hears; a P3 packet's pulse 0 is
	// centred 25.5 samples (at 9600 Hz) after the slot's word-0 centre; CS
	// carriers ~2750 and P3 data ~500 per carrier at amplitude 4400.
	static constexpr double kP3CalleeReply = 0.8848, kP3Off = 25.5 / 9600.0;
	static constexpr float kP3CsGain = 0.625f, kP3DataGain = 0.115f;
	double p3_t0_of(int64_t slot) const { return double(slot) + 0.8 * T_ + kP3Off * cfg_.fs; }
	int64_t p3_slot(const p3::RxPacket &r) const { return int64_t(std::llround(r.t0 - 0.8 * T_ - kP3Off * cfg_.fs)); }
	// Where a P3 data burst starts and its pulse 0 lies within it. Default:
	// the start rounded to a sample, the lead fixed (pulse 0 up to half a
	// sample off the nominal instant). Test: P3_TX_SHIFT=S (samples, may be
	// fractional) moves pulse 0 by S from the nominal instant exactly, the
	// fraction carried in the lead.
	static double p3_tx_shift()
	{
		static const double v = std::getenv("P3_TX_SHIFT") ? std::atof(std::getenv("P3_TX_SHIFT")) : std::nan("");
		return v;
	}
	void p3_place(double pulse0, double lead0, int64_t &start, double &lead) const
	{
		const double sh = p3_tx_shift();
		if (std::isnan(sh)) { start = int64_t(std::llround(pulse0 - lead0 * cfg_.fs)); lead = lead0; return; }
		const double p = pulse0 + sh;
		start = int64_t(std::floor(p - lead0 * cfg_.fs));
		lead = (p - double(start)) / cfg_.fs;
	}
	// Fix 225: where a long packet's pulse 0 lies after slot + 0.8 T. The reference unit's long packets keep
	// the pulse grid of its short packets at that level (its own audio: short and long bursts share
	// one grid phase, SL4-SL6). Ours had no
	// offset (fix 13's SL3 reading), 25.5 samples (2.7 ms) early at SL6: the reference unit then
	// failed or passed our long packets by our level and carrier phase. Measured against the
	// reference unit (emulator, SL6 and SL5): best at 14-20 samples, so the level's short offset less
	// 6.5 samples (SL5/6 19, SL4 17.5, SL3 17.25; SL2 0 as before). P3_LONG_SHIFT=S: S samples
	// at every level (test); P3_LONG_OLD=1: none (before fix 225).
	double p3_long_offset_s(int level) const
	{
		static const char *e = std::getenv("P3_LONG_SHIFT");
		static const bool old = std::getenv("P3_LONG_OLD") != nullptr;
		if (e) return std::atof(e) / 9600.0;
		if (old || level <= 2) return 0.0;
		return std::max(0.0, p3::level_offset_s(level) - 6.5 / 9600.0);
	}
	double p3_pulse0(int64_t slot, int level, bool lng) const
	{
		if (level == 5 && !lng) return p3_t0_of(slot);
		return double(slot) + 0.8 * T_ + (lng ? p3_long_offset_s(level) : p3::level_offset_s(level)) * cfg_.fs;
	}
	void queue_p3(int64_t slot, const std::vector<uint8_t> &words_in);
	// A P3 packet in the slot `slot` (absolute t0 in the result).
	// search_ms: coarse timing search half-width (0 = slot already known
	// from the previous packet: fine search only, ~30 ms of CPU).
	// Speed levels 2-4: words -> audio, pulse 0 at the level's offset from the slot.
	// P3_DATA_SCALE / P1_FSK_SCALE (below): our P3 data / P1 FSK levels x
	// a factor (tests, TODO 0b). Our FSK peaks at the PSK peak (RMS 3,058), the
	// emulated firmware's at 1,713 (RMS 1,209, 3.6 dB under its P2 data's
	// 1,843) [bb-ext, D/A]; changing ours moved 0b's stalls around without a
	// trend (2026-10-20: FSK 0.95 / 0.85 cured the 4 stalling texts, 0.90 not,
	// and other scenarios flipped the same way), so the levels stay as they were.
	float p3_data_amp() const { static const float k = std::getenv("P3_DATA_SCALE") ? float(std::atof(std::getenv("P3_DATA_SCALE"))) : 1.0f; return cfg_.amplitude * k; }
	static float fsk_scale() { static const float k = std::getenv("P1_FSK_SCALE") ? float(std::atof(std::getenv("P1_FSK_SCALE"))) : 1.0f; return k; }
	static float lv_gain(int level) { return level == 2 ? 0.354f : level == 3 ? 0.162f : level == 4 ? 0.149f : kP3DataGain; }
	// Test hook (fix 263): P3_TEST_TX_SKEW="AT:UNTIL[:STEP]": from AT to UNTIL s of audio our P3 data packets
	// (levels 2-6) go out with their words' phase steps replaced by skewed ones (90 % within one step of
	// a centre that moves up one step every 10 s from STEP, default 1; the rest random; the same packet
	// gives the same words), and a T does not lower our level (as the reference unit on the air, which
	// went on sending multi-carrier packets while we expected SL1): bursts that do not decode and whose
	// spectrum reads off the true shift by several Hz, wandering, with a comb match like a real burst's
	// (the on-air readings of 2026-11-18).
	int p3_test_skew_step() const;                     // the centre step now, 0: the hook is not active
	std::vector<uint8_t> skew_w_;                     // (the test hook's words)
	const std::vector<uint8_t> &p3_test_skew(const std::vector<uint8_t> &words);
	void queue_lv(int64_t slot, int level, const std::vector<uint8_t> &words_in, bool lng = false);
	// The decode runs on a worker thread, launched once the packet is in
	// (slot + kP3Launch) and collected when the answer is due; in real time
	// it then never stalls the audio loop (a full decode is 20-50 ms).
	static constexpr double kP3Launch = 0.852;
	p3::RxPacket find_p3(int64_t slot, double search_ms)
	{
		if (p3_fut_.valid() && p3_fut_slot_ == slot) return p3_fut_.get();
		launch_p3(slot, search_ms);
		return p3_fut_.get();
	}
	// Decodes we stopped waiting for (their results are dropped when they
	// finish), never waited for on the audio loop.
	std::vector<std::future<p3::RxPacket>> stale_futs_;
	std::chrono::steady_clock::time_point p3_launch_wall_;
	// Timing of a late P3 decode (log only, 2026-11-18: on the Pi one late decode starts a computer
	// back-off; was it compute or waiting?): when each level's decode started and ended against the
	// launch, and when the result came against the deadline it missed. Logged once it is done.
	struct DecodeTimes {
		static constexpr size_t kMax = 8;
		int64_t launch = 0; int n = 0; int lv[kMax] = {};
		std::atomic<int64_t> s[kMax] = {}, e[kMax] = {};
	};
	std::shared_ptr<DecodeTimes> p3_times_, late_watch_;
	std::shared_ptr<std::atomic<int64_t>> late_watch_done_;
	int64_t late_watch_dl_ = 0; int late_watch_lv_ = 0;
	void late_watch_poll();
	void reap_stale_decodes()
	{
		late_watch_poll();
		for (size_t i = 0; i < stale_futs_.size();)
			if (stale_futs_[i].wait_for(std::chrono::seconds(0)) == std::future_status::ready) stale_futs_.erase(stale_futs_.begin() + long(i));
			else i++;
	}
	// The levels a P3 packet is tried at, in order: the expected level, then
	// one lower (the sender steps down after failures); at SL6 also SL5;
	// one higher after our T (the sender steps down on the first T and then
	// only after 5 more). SL5 first when expected (and
	// alone below SL2).
	// Fix 219: re-acquiring (two misses or more), the level last decoded is tried too: our requests
	// for a lower level went out where we expected the packets, not where they came, and the sender
	// may not have read them (a slip test channel, 36/30/10 ms: it stayed at SL6 while we looked at 4/3).
	int p3_last_ok_level_ = 0;
	bool p3_t_untaken_ = false;                      // fix 219: this decode came above the level our T asked for
	// slot: the packet's (the CPU fallback is decided once per packet); trimmed: set when it left levels out.
	std::vector<int> p3_levels_now(int64_t slot, bool *trimmed = nullptr) const;
	// CPU fallback (TODO 5): the other levels are tried in case the sender changed level unasked (it
	// steps down after failures, or did not take our request). While the packets decode at the level
	// expected, with no failure and no request of ours in flight, only that level is tried when the CPU
	// is short: on one Pi 4 core a level's early timing search and plan are 120-250 ms of CPU and its
	// decode 7-26 ms, all spent on a level the packet is not at. A packet the sender did send at another
	// level then fails once and the next is tried at every level again (misses_ > 0).
	bool p3_fb_steady() const { return misses_ == 0 && !rx_t_state_ && p3_last_ok_level_ == p3_rx_level_; }
	int p3l_last_ok_ = 0;                            // the level of the last long packet decoded
	// The CPU fallbacks' state (set_cpu_health). CPU_FALLBACK_OLD=1: never (the old way); CPU_FALLBACK_FORCE=1:
	// always while CPUadapt is on (a test: what a fallback costs on a computer that keeps up). Real time only.
	static constexpr double kCpuFbHoldS = 60.0;
	int cpu_health_ = 0;
	int64_t cpu_fb_until_ = -1;                      // fall back until this sample (the health not OK, plus the hold)
	int64_t fb_at_[1] = {-1};                        // when each fallback last changed something (FB_* bit k)
	mutable int64_t fb_slot_ = -1;
	mutable bool fb_slot_on_ = false;
	static int cpu_fb_env() { static const int v = std::getenv("CPU_FALLBACK_OLD") ? 0 : std::getenv("CPU_FALLBACK_FORCE") ? 1 : -1; return v; }
	bool cpu_fb_now() const
	{
		if (!cfg_.realtime || !cfg_.p3_request_long || cpu_fb_env() == 0) return false;
		return cpu_fb_env() == 1 || n_ < cpu_fb_until_;
	}
	bool cpu_fb_at(int64_t slot) const   // decided once per packet slot (its search, look and decode agree)
	{
		if (slot != fb_slot_) { fb_slot_ = slot; fb_slot_on_ = cpu_fb_now(); }
		return fb_slot_on_;
	}
	void fb_note(int k, long &count)
	{
		if (debug_ && (fb_at_[k] < 0 || n_ - fb_at_[k] >= samples(kCpuFbHoldS)))   // the host reports it ([cpu])
			log("CPU fallback: P3 packets tried at the expected level only");
		fb_at_[k] = n_; count++;
	}

	static std::vector<int> p3_try_levels(int lv, bool up);
	// One level's timing (SL5: Sync5; SL2-4, SL6: LvSync) and the data-independent part of its demodulation.
	struct P3LvSync { int lv = 0; p3::Sync5 s5; p3::LvSync s; std::shared_ptr<const p3::DemodPlan> plan5; std::shared_ptr<const p3::FlatPlan> plan; };
	// Audio a..b of the ring, zero where not (yet) received.
	std::vector<float> p3_window(int64_t a, int64_t b) const;
	double p3_rx_df_ = 0;                            // the peer's offset as the P3 decoders see it, Hz (fix 49)
	// Fix 213b: p3_rx_df_ was set only by a decoded B1/A2/break-in. When none decoded (bench
	// f216-15: the reference unit called at -18 Hz, its 200 Bd part unread (CS4), A2 not decoded) the P3
	// levels 2-6 were read at the nominal tones for the whole link (SL2-SL4 fail at ~20 Hz, fix 70)
	// although the set-up's FSK had measured the offset. Seeded from the AFC (-afc_hz(), the
	// break-in path's convention) at the first P3 decode launch without a measurement.
	// P3_NO_DF_SEED: off.
	bool p3_df_set_ = false;
	void p3_df_seed();
	// ---- Re-acquiring the peer's frequency after a step (2026-11-14) ----
	// The P3 decoders take the receive shift as given and remove only a residual (+-12.5 Hz at
	// SL4-6); the shift itself was followed only from packets that decoded (by readings under 12 Hz),
	// and the control-signal AFC takes the alias nearest its tracked value (half a turn, 50 Hz). After
	// a sudden step (an operator retuning, a VFO jump) of 30 Hz or more nothing decoded again and
	// nothing measured the new offset: ALSA loopback, +30/-40/+50 Hz on the sender at 14-19 s, the
	// receiver asked down to SL1 and the link carried nothing until it timed out.
	// P2 the same: its data packets' bias is unwrapped over the full turn (100 Hz), so a +50 Hz step
	// was followed to -50 Hz (loopback: 1,966 of 6,000 characters in 120 s).
	// Now each burst that fails (a data packet as receiver, a control signal as sender) is measured
	// from its power spectrum (freq_search.hpp: the carrier comb's shift, a few ms on a worker, read at
	// the next look); two failed bursts in a row that agree, both clearly a burst of ours (match), and
	// more than 8 Hz (data) / 15 Hz (control signals: their own reading resolves +-25 Hz) from the
	// shift in use set the shift: the P3 receive shift and the AFC (SL1 and control signals are read
	// at the AFC's tones). Nothing we send changes (the caller's transmit follow then sees the
	// AFC's readings as before). SIGNUM_FSTEP_OLD=1: off (the old behaviour).
	static bool fstep_old() { static const bool v = std::getenv("SIGNUM_FSTEP_OLD") != nullptr; return v; }
	// Fix 261 (on the air 2026-11-18, TODO ff 2): the P3 callee's packets failed for a while after a
	// computer back-off (t2s-pcall, t2s-pcall.3), and each failed packet's spectrum was searched. The
	// 18-carrier comb (template 2) matched the radio's receive pass band itself at 0.50-0.52, over the
	// 0.35 threshold: band-limited noise looked like a burst (freq_search.hpp; on white noise it
	// matches under 0.2). Two such readings 5 Hz apart set the shift, and
	// the next search, kept to +-70 Hz around the shift in use, started from there: -18 -> -10 -> +8 ->
	// +17 -> +29 -> +40 Hz, nothing decoded again, the link timed out. Lockstep with band-limited noise:
	// -34 -> +24 -> +35 -> +16 -> +28 Hz (200-2600 Hz), -34 -> -94 -> -157
	// Hz (300-2400 Hz) while the peer was faded out. Now
	//   (1) the comb templates (1 and 2, the ones that repeat every 120 Hz and are searched around a
	//       centre) count only when their comb match (envelopes removed) is at least kFsComb: band-limited
	//       noise of any shape stays under ~0.27, a P3 burst at 10 dB SNR is over it (at 3 dB mostly);
	//   (2) they are searched around the shift at which a P3 data packet last decoded (or the set-up's,
	//       before any), not around the shift in use, so readings cannot chain: whatever is accepted lies
	//       within +-70 Hz of a shift that was confirmed by a decode.
	// The two-carrier templates (0: P3 SL1 and control signals; 3: P2) are absolute (searched around 0)
	// and band noise matches them under 0.25: unchanged. P3_RXSHIFT_OLD=1: as before.
	static bool rxshift_old() { static const bool v = std::getenv("P3_RXSHIFT_OLD") != nullptr; return v; }
	static constexpr double kFsComb = 0.30;
	double fs_anchor_ = 0;                           // fix 261: the shift (Hz as heard) at the last decoded P3 data packet
	bool fs_anchor_ok_ = false;
	void fs_anchor_set() { fs_anchor_ = -p3_rx_df_; fs_anchor_ok_ = true; }
	// Fix 263 (on the air 2026-11-18, second session: fix 261 did not hold; TODO ff 2): the unit
	// calling at P3 with `--verify --sign`, 5 of 18 runs still walked (-18 -> -7 -> +9 -> +21 -> -83 ->
	// +23 ...), every applied reading with comb 0.30-0.50: real P3 bursts that we did not decode
	// (we expected SL1 after a computer back-off; the unit kept sending multi-carrier packets), read
	// 8-16 Hz above the true shift and wandering. A burst's spectrum cannot tell a frequency offset
	// from a bias in its phase steps: P3 words are phase steps of 1/16 turn per pulse at 100 Bd, so a
	// constant step is exactly a 6.25 Hz offset, and a packet whose steps are not evenly spread (its
	// content; a repeat has the same) moves the comb's peak while keeping its shape (measured: 90 %
	// of steps in 0-2 reads ~6 Hz off at match 0.31-0.36, comb 0.33-0.38; spread steps within 3.6 Hz).
	// So no spectral score can confirm a reading: only a decode can. Now a P3 data-packet reading
	// (two that agree, as before) is a CANDIDATE, tried and kept only if a packet decodes:
	//   (1) tried only after fs_wait_ failed data packets in a row since the last decode: kFsMinFails (2,
	//       what two agreeing readings take anyway, so a real step is re-acquired as fast as before),
	//       doubled after each try that failed (4, 8, 16, at most kFsMaxWait);
	//   (2) a peak within kFsEdge of the search window's edge is not a reading (on air the -81...-84 Hz
	//       readings sat 4-7 Hz inside the -88 Hz edge: the comb's 120 Hz repeat, not a carrier);
	//   (3) a candidate more than kFsCap from the last decoded shift needs three agreeing readings (HF
	//       drift is well under 1 Hz per packet; only a retune jumps);
	//   (4) the candidate is applied for at most kFsTrial failed packets: a packet decoded at level 2 or
	//       above confirms it (it becomes the decoded shift); otherwise the shift and AFC go back to
	//       what they were (a wrong try costs a working link at most kFsTrial packets, then (1) spaces
	//       the next one out);
	//   (5) every candidate not taken and every trial that failed is logged with its scores.
	// P3_RXSHIFT_261=1: fix 261 as it was (applied at once); P3_RXSHIFT_OLD=1: before fix 261.
	static bool rxshift_261() { static const bool v = std::getenv("P3_RXSHIFT_261") != nullptr; return v; }
	bool fs_trial_on() const { return !rxshift_old() && !rxshift_261(); }
	static constexpr int kFsMinFails = 2, kFsTrial = 3, kFsMaxWait = 32;
	static constexpr double kFsEdge = 10.0, kFsCap = 15.0, kFsNearDec = 3.0;
	int fs_fails_ = 0;                               // fix 263: failed P3 data packets since the last decode
	int fs_wait_ = kFsMinFails;                      // fix 263: failed packets needed before the next try
	bool fs_trial_ = false;                          // fix 263: a candidate is being tried
	int fs_trial_fails_ = 0;                         // fix 263: failed packets at the candidate
	double fs_trial_hz_ = 0, fs_trial_was_df_ = 0, fs_trial_was_afc_ = 0;
	struct FsResult { int64_t slot = -1; int kind = 0; bool ok = false; double hz = 0, match = 0, comb = 0; int tmpl = -1; double ms = 0; bool edge = false; double centre = 0; };
	FsResult fs_prev2_;                              // fix 263: the reading before fs_prev_ (three agreeing for a large step)
	// A P3 data packet decoded at `level` (fix 263; fix 261's anchor): a trial is confirmed by a level
	// 2-6 decode (those are read at the receive shift; SL1 is read at the AFC's tones and leaves a
	// trial as it is).
	void fs_decoded(int level);
	// A P3 data packet failed (fix 263): a trial that has had kFsTrial failures goes back.
	void fs_failed();
	std::shared_future<FsResult> fs_fut_;
	FsResult fs_prev_;                               // the last estimate that differed from the shift in use
	int fs_reseeds_ = 0;                             // per link (logged)
	static constexpr double kFsMatch = 0.35;         // noise alone matches the comb at < 0.2
	// The templates: 0 P3 control signals and SL1 (two carriers, the narrow pulse), 1 SL2 (six carriers,
	// the narrow pulse), 2 SL3-6 (the 18-carrier comb, the flat-top pulse), 3 P2 data and control
	// signals (our two tones, the root-raised-cosine pulse).
	static const fmodem::fsearch::PulsePower &fs_p2_pulse()
	{
		static const fmodem::fsearch::PulsePower pw = [] {
			float h[2 * 96 + 1];
			for (int i = 0; i <= 2 * 96; i++) h[i] = float(p2::rrc1(i / 96.0));
			return fmodem::fsearch::PulsePower(h, 2 * 96);
		}();
		return pw;
	}
	static FsResult fs_measure(const std::vector<float> &x, double fs, double centre, int kind, int64_t slot, bool p3, double lo, double hi, bool old);
	// The received audio of [a, b), unshifted (at most 1.3 s: enough for the spectrum).
	std::vector<float> fs_window(int64_t a, int64_t b) const
	{
		b = std::min(b, a + samples(1.3));
		std::vector<float> x(size_t(std::max<int64_t>(b - a, 1)), 0.0f);
		if (a >= 0 && n_ - a < kRing - 2)
			for (int64_t t = a; t < b && t < n_; t++) x[size_t(t - a)] = raw_[size_t(t % kRing)];
		return x;
	}
	// The offset in use, Hz as heard: P3 data packets are read at the receive shift, everything else
	// (P3 SL1, control signals, P2) at the AFC's tones.
	double fs_in_use(int kind) const { return (p3_ && kind == 0) ? -p3_rx_df_ : afc_hz(); }
	// kind 0: a data packet that failed (slot `slot`, [a, b)); 1: a control signal not read.
	void fs_submit(int64_t slot, int64_t a, int64_t b, int kind);
	// A finished estimate, if there is one (no wait): two in a row that agree set the shift.
	void fs_poll();
	static double kLvBiasHz_() { return std::getenv("P3_LVBIAS_HZ") ? std::atof(std::getenv("P3_LVBIAS_HZ")) : 6.25; }
	static P3LvSync p3_sync_lv(int l, const float *x, long n, double fs, double base, double search_ms)
	{
		P3LvSync r; r.lv = l;
		if (l == 5) r.s5 = p3::sync_at(x, n, fs, base + kP3Off * fs, search_ms);
		else r.s = p3::sync_level(l, x, n, fs, base + p3::level_offset_s(l) * fs, search_ms > 0 ? search_ms : 1.0);
		return r;
	}
	// Timing search, run once the headers are in (EV_P3_SYNC): it needs only
	// the first 12 pulses and is most of a decode's cost (36-108 ms per level
	// on a 3.3 GHz i5), which would not fit between the packet's end and our
	// answer (~45 ms).
	// Fix 236: one search per level, on several workers, each level's result published as soon as it
	// is done (a level's decode waits for its own level only). It was one task searching every
	// candidate level in turn: on the Pi 4 ~100-150 ms per level, so with 5 candidates (after a T,
	// with the last level decoded beside them) it was still running at the decode launch 0.55 s later
	// and every level waited ~14 ms for it (f232-1 15:38:09: 25.4 ms against ~23.7 ms, computer
	// back-off). The levels go round-robin, in the order tried, into at most (decode workers - 1)
	// groups (the Pi 4: 3), so one core stays free for the rest of the station. Same searches, same
	// windows, same plans: the decodes are unchanged. P3_SYNC_GROUPS=n: n groups (1: one task, as before).
	struct P3EarlySync { int64_t slot = -1; std::vector<int> lvs; double df = 0; std::vector<std::pair<int, std::shared_future<P3LvSync>>> f; };
	P3EarlySync p3_early_;
	static constexpr double kP3Sync = 0.30;
	// One core for a station's decodes (2026-11-15; the Pi 4's 1k' calm pair, each station pinned to a
	// core): the workers are SCHED_FIFO 20 and do not time-slice, so the early timing searches (a
	// group's levels one after the other: on the Pi 4 SL4 + SL5 418-472 ms, SL3 ~150, SL2 ~120) ran
	// group by group to ~1.04 s after the slot, and the level decodes queued at 0.852 s waited for
	// all of them: every packet late, the computer back-off down to SL1. Now the work is ordered by
	// when it is needed: each level's early search runs at FIFO 18 less its place in the levels
	// tried (the first level first) on its own workers (sync_pool_), an SL1 try started only in case
	// (receiving above SL2) at 19, and the decodes at 20; and the task that takes the first level
	// that decodes runs a level's decode itself when no worker has started it yet (a worker that
	// then takes it does nothing), so it never waits behind a queue. Same tasks, same inputs, same
	// order of choice: no decode changes; with several free cores nothing waits
	// either way. Real-time priority only (FifoPrioScope). P3_SCHED_OLD=1: the old way.
	static bool p3_sched_old() { static const bool v = std::getenv("P3_SCHED_OLD") != nullptr; return v; }
	static int p3_prio(int below) { return std::max(1, par::worker_prio() - below); }
	int p3_sync_groups(size_t nlv) const
	{
		static const int env = std::getenv("P3_SYNC_GROUPS") ? std::atoi(std::getenv("P3_SYNC_GROUPS")) : 0;
		const int g = env > 0 ? env : std::max(1, p3_sched_old() ? pool_.size() - 1 : sync_pool_.size());
		return std::max(1, std::min(g, int(nlv)));
	}
	void launch_p3_sync(int64_t slot, double search_ms);
	// The SL1 try, once an SL1 packet is fully in (slot + 0.83 s; EV_P3_SL1 at
	// 0.835 s, ~17 ms before the level decodes launch): on the RF bench's 4
	// cores, started with them it was still "not ready" when collected ~28 ms
	// later (KB2PCN-8, 80 m, 2026-10-15 22:03; fix 40 follow-up).
	// The SL1 try on a worker; with P2_DEBUG its time ("P3 SL1 worker: ...", fix 216b).
	// in_case: started before the main decode is known to fail while receiving above SL2 (it then
	// runs below the level decodes on a shared core; above)
	std::future<RxPacket> submit_sl1(int64_t slot, bool in_case = false);
	int p3_since_fail_ = 1 << 20;                    // P3 packets decoded since the last failed one (fix 216b)
	static constexpr int kSl1Recent = 4;
	void launch_sl1(int64_t slot);
	// P3 memory ARQ over failed copies at the expected level (2026-10-18;
	// SL1 has its own, P2-style): combined per word as differential products
	// (maximum-ratio combining for DPSK). Offline (a bench tool, white
	// noise, 3 copies): SL3 noise 3500 0 -> 73 %, SL4 noise 2500 0 -> 55 %.
	struct MqCopy { int level; std::vector<std::complex<double>> d0, d1; };
	std::vector<MqCopy> mq3_;
	static constexpr size_t kMq3Max = 2;            // stored copies (with the newest: 3)
	void mq3_clear() { mq3_.clear(); }
	void launch_p3(int64_t slot, double search_ms);
	// Does the 2-carrier burst near `exp` carry words 1..n of `w` (either
	// carrier order)? start = its word-0 slot.
	// Set-up words heard late (a Signum change, 2026-11-13): the burst's start is looked for from
	// 40 ms before to 300 ms after `exp` as before, but its pulses are taken from everything heard
	// so far (the caller's B1 look now comes at exp + 0.335 s, kP3B1Look), and only pulses already
	// heard are scored. Two stations of ours on the ALSA loopback: the callee's B1 arrives ~111 ms
	// after exp (each side's audio path, ~45 ms one way, plus the callee's reply timing). The look
	// at exp + 0.30 s had words 1-18 of 24 in; words 19-24 were fitted to the zeros past the last
	// sample and counted as misses, so the match read 39 or 40 of 48 against 40 needed (the logged
	// "best 0.81" = 39/48), decided by one sample of where the burst fell: about one set-up in four
	// (three in four with four pairs at once) ended in "P3 set-up timeout". Two Pis (65 ms round
	// trip each) would put B1 ~130 ms late: 15 words heard at 0.30 s, too few to match at all.
	// The reference unit's B1 comes 3-9 ms after exp (bench), every pulse in by then: unchanged.
	// P3_SETUP_HEARD_OFF=1: as before (pulses to exp + 0.30 s, all scored, the look at 0.30 s).
	static bool setup_heard_off() { static const bool v = std::getenv("P3_SETUP_HEARD_OFF") != nullptr; return v; }
	static constexpr double kP3B1Look = 0.335;           // the caller's B1 look, s after exp (the A1 repeat still 30 ms ahead)
	double p3_b1_look() const { return setup_heard_off() ? 0.30 : kP3B1Look; }
	// heard_upto >= 0: read as if heard up to there (fix 243's late look at a burst before our own last one).
	bool mw_partial_ = false;                       // fix 257: match_words' last miss was a late burst's words, too few heard
	int64_t mw_partial_at_ = -1;                     // (its word-0 slot)
	bool match_words(int64_t exp, const uint8_t *w, int n, double &start, int64_t heard_upto = -1);
	// Fix 252: the callee's A1 decode (fix 218: B1 only for an A1 that decodes) on a worker. It ran
	// on the audio loop at A1 + 0.86 s, ~25 ms before our B1 is due: on a Raspberry Pi 4 it took 32-35
	// ms (65-95 ms with the station on one core), a playback underrun that cut or lost the B1. Now it
	// is launched once the A1 is in (+0.84 s, as the SL1 try of data packets) and polled every 2 ms;
	// done by our B1's leading tail, B1 goes as before. Done later: the caller, having heard no B1,
	// sends its A1 again a cycle on (as it does for a B1 lost on the air), and our B1 answers that one
	// (the A1 already decoded: nothing more to confirm). Not decoded: as before (step 13). Real time
	// only (lockstep runs as before). P3_A1_INLINE_OLD=1: the old way.
	bool a1_async() const { static const bool old = std::getenv("P3_A1_INLINE_OLD") != nullptr; return cfg_.realtime && !old && pool_.size() > 0; }
	// Fix 271: the A1 decoded without a header match (set-up step 13) on a worker, and what follows it.
	static bool setup_late_old() { static const bool old = std::getenv("P3_SETUP_LATE_OLD") != nullptr; return old; }
	std::future<RxPacket> a1n_fut_;
	int64_t a1n_slot_ = -1;
	// A good SL1 decode of slot `ref` is the caller's A1: B1 at its own start, or at the caller's next A1 when
	// that instant has passed (fix 271). Returns false when the slot held no A1.
	bool a1_nomatch_decoded(const RxPacket &q, int64_t ref);
	// Neither A1 nor the caller's set-up packet in slot `ref` (set-up step 13's end; fix 218)
	void a1_none(int64_t ref);
	std::future<RxPacket> a1d_fut_;
	int64_t a1d_slot_ = -1;
	bool a1d_late_ = false;
	// Fix 252: the callee's set-up and break-in looks before its next burst. The look for the
	// caller's answer to our burst at `burst` (B1, the greeting, our break-in) came at +0.971 s (the
	// answer's place) + 0.30 s = +1.271 s, but whatever it decides (B1 again, the greeting again or at
	// a new level, the break-in again) goes out at +1.25 s (one cycle), so that burst was queued 21 ms
	// after its start plus its leading tail, and its first ~33 ms were never sent. Two of our stations
	// on the ALSA loopback (a Raspberry Pi 4): every B1 sent again lost its first three words, the
	// caller never matched one ("best 0.64 at word -2"), and a first B1 lost (the callee's audio loop
	// stalled at the A1 decode) ended the set-up. The caller's answer arrives at +0.965 s (measured,
	// two Signums) and lasts ~0.13 s; the look now comes just before our next burst's leading tail
	// (as fix 243's cap and fix 250's look deadline), +1.235 s at 1.25 s cycles. The caller's own
	// looks (+0.879 + 0.30 s) were always before its next burst. P3_CALLEE_LOOK_OLD=1: the old way.
	// The caller's look for the answer to its A2 or its break-in at `burst` (+0.8848 + 0.30 s). Fix 257:
	// later by our round trip's excess over the default path, at most to just before our next burst,
	// as fix 243's data-phase look (reply_check_at): the answer reaches us that much later. With the
	// default path (rt_latency <= 0) unchanged. P3_LONG_PATH_LOOK_OFF=1: as before.
	int64_t caller_look_at(int64_t burst) const
	{
		const int64_t usual = burst + samples(kP3CalleeReply + 0.30);
		if (!cfg_.realtime || cfg_.rt_latency_s <= 0.0f || std::getenv("P3_LONG_PATH_LOOK_OFF")) return usual;
		return std::max(usual, std::min(usual + samples(cfg_.rt_latency_s), p3_look_deadline(burst + samples(cyc_s()))));
	}
	int64_t callee_look_at(int64_t burst) const
	{
		static const bool old = std::getenv("P3_CALLEE_LOOK_OLD") != nullptr;
		const int64_t usual = burst + samples(master_seen_s() + 0.30);
		if (old) return usual;
		return std::min(usual, p3_look_deadline(burst + samples(cyc_s())));   // (fix 265: our next burst's own lead)
	}
	// The callee's first P3 packet: its connect text, counter 3, OVER.
	int greet_ep_cs_ = -1, greet_fails_ = 0, greet_t_run_ = 0;   // the caller's episode after our B1 (set-up step 12)
	// Fix 210: the caller's receiver after it fell back from its B1 answer (T or S), as a model of
	// what it expects: fb_[k] = {level, T letters counted, lowest level, alive}; two while which
	// letter it fell back from is unknown (both tried in turn until a failure rules one out).
	// greet_guess_ = the B1 answer was taken from a soft reading only; greet_skip_p_ = the next
	// letter may answer the greeting right after our B1 (no information); b1_resends_ / b1_soft_ =
	// our B1 sent again after a look with no letter, the soft letter those looks read.
	struct FbModel { int lvl = 0, t_run = 0, floor = 1; bool alive = false; };
	FbModel fb_[2];                                   // [0] fell back from T, [1] from S
	int fb_next_ = 0, b1_resends_ = 0, b1_soft_ = -1;
	int greet_scan_ = -1;                             // >= 0: the model lost, every level tried in turn (index into kScan)
	bool greet_guess_ = false, greet_skip_p_ = false;
	void b1_reset() { greet_sent_ = false; a2_quiet_at_ = -1; b1_resends_ = 0; b1_soft_ = -1; greet_guess_ = false; greet_skip_p_ = false; fb_[0].alive = fb_[1].alive = false; greet_scan_ = -1; }
	// The last resort: the caller's letters fit no model (e.g. its T run had begun before we heard
	// a letter). While it fails our packets its expected level holds for at least 3 cycles (P, P
	// before its first T, then 5 T per level) and ends at SL2 or SL1, so a sweep of the short
	// levels meets it. Upwards: a packet one level above the one it expects (its previous level)
	// made it refuse the next good one (emulator: after S, SL2 then SL1 refused, SL1 again taken;
	// SL3 then SL1 taken).
	void greet_scan_next(const char *why);
	void fb_start(bool from_t, bool from_s)
	{
		fb_[0] = FbModel{4, 0, 2, from_t};          // after T: SL4, never below SL2 (emulator)
		fb_[1] = FbModel{2, 0, 1, from_s};          // after S: SL2, then SL1
		fb_next_ = from_t ? 0 : 1;
		p3_level_ = fb_[fb_next_].lvl; greet_fails_ = 0;
	}
	bool fb_on() const { return fb_[0].alive || fb_[1].alive; }
	// A letter other than A2 after our greeting at `sent`: rule out a model that expected that level
	// (unless the letter carries no information), step each on its T run, choose the next level.
	void fb_letter(int cs, int sent, bool informative);
	int p3_ans_rep_ = 0;                                          // fix 265: the break-in answer's letter repeated, in a row
	int p3_bk_tries_ = 0, p3_bk_guess_ = -1;                      // fix 265: our break-in sent again (its answer unread), and what those looks read
	int p3_guess_rep_ = -1;                                       // fix 276: our level guessed by fix 265 (>= 0: repeat P's since, -1: not guessed)
	// Fix 277: a look after our P3 break-in that found the peer's data packet, not an answer. The peer still sends: it has not
	// taken our break-in, so that look is no unread answer for fix 265. On the air 2026-11-23 (link 22, 80 m, an RMS) the five
	// looks fix 265 counted as unread answers read hard 0.38-0.45, soft 0.23-0.28: what a P3 data packet reads as a control
	// signal (a recorded SL3 packet: 0.40-0.52 / 0.22-0.39), not a control signal read at the wrong timing (the recorded
	// answers to our break-ins with a carrier 2-4 pulses off, and with both carriers off: 0.60-0.74). The peer had not taken
	// our break-in; its P that followed was its first answer (level 1), and fix 265 took it for "the missed answer's less one"
	// (level 4) at a receiver on level 1: the 71 repeats fix 276 stops. Told apart by the spectrum: a data packet at level 2 or
	// more has the far tones (k 3, 7, 10, 14: in every level 2-6 packet, two tones or more from the control-signal tones k 5
	// and 12) as strong as the control-signal tones, a control signal has nothing there (recorded: far / CS tones 1.0-1.5 in
	// SL3 packets, 0.08 in the answers); and the far tones must be within 6 dB of those of the packet we decoded just before
	// breaking in, so that noise alone is no packet. Only after a packet at level 2 or more (SL1 packets use the control
	// signal's tones). P3_BK_PK_OLD=1: every look that reads nothing counts as an unread answer, as before.
	double p3_bk_ref_far_ = -1;                                   // fix 277: far-tone power of the peer's packet before our break-in (-1: none)
	static constexpr int kP3FarTones[4] = {3, 7, 10, 14}, kP3CsTones[2] = {5, 12};
	// Mean power per tone over [from, to) of the P3 tones ks (480 + 120 k Hz), in 10 ms blocks mixed to each tone (a block's
	// response falls 28 dB 240 Hz off and 0.4 dB 15 Hz off: the peer's offset is not needed). From the receive ring.
	double p3_tone_power(int64_t from, int64_t to, const int *ks, int nk) const;
	// Fix 277: did the look at `exp` (our break-in's answer) find the peer's data packet?
	bool p3_bk_peer_packet(int64_t exp);
	bool greet_long_ = false;                                     // the greeting as an SL1 long packet (B1 answered with R)
	// Set-up step 12's A2, at `a2`: our answer sets the caller's level, then we receive.
	void take_setup_a2(int64_t a2);
	// Fix 235: an A2 whose header did not match. The caller sends A2 (a break-in) once it has
	// taken our greeting, and repeats it every 1.25 s cycle until it hears a letter to it, for its
	// whole set-up timeout (emulated firmware: our first A2 match made to fail, the repeat one
	// cycle later; the bench f232-2: the same burst read "cs - score 0.714 soft T 0.428" every
	// cycle for 46 s, the emulated A2 reading exactly so). Our next burst (greeting or B1 again,
	// 0.28 s after the A2's start) covers its data: we cannot hear while we send. So a look with
	// no letter but a burst there (score >= 0.6; nothing and noise read 0.36-0.52 on the bench)
	// costs one cycle of silence; the slot is then decoded as a break-in (on a worker in real
	// time: fix 213c's A1 decode held the Pi's audio loop 141 ms) and a good one is answered, in
	// time or at its repeat; otherwise the greeting goes again. Never two silent cycles in a row.
	// The carrier order keeps the cycle grid (fix 222). P3_NO_A2_WATCH=1: off.
	int64_t a2d_slot_ = -1, a2_quiet_at_ = -1;
	bool greet_sent_ = false;
	double look_score_ = 0;
	std::future<RxPacket> a2d_fut_;
	bool match_a2(int64_t exp, double &st);
	bool a2_partial_ = false;                       // fix 257: the last A2 look saw a late A2's first words
	int64_t a2_partial_at_ = -1, a2_late_at_ = -1;  // (where it began; the slot a2_decode reads)
	bool a2_quiet(int64_t exp);
	void a2_decode(int64_t exp);
	void send_greeting(int64_t slot);
	// The set-up letters choose the next sender's level (emulated firmware):
	// T or Q -> 5, S -> 3, P or R -> 1.
	static int letter_level(int cs) { return (cs == p2arq::CS_T || cs == p2arq::CS_Q) ? 5 : cs == p2arq::CS_S ? 3 : 1; }
	static int level_letter(int lv) { return lv >= 5 ? p2arq::CS_T : lv >= 3 ? p2arq::CS_S : p2arq::CS_P; }
	// The 3 characters a P3 break-in (B1, A2, changeover) carries: decoded and
	// delivered once the whole burst is in (it is recognised from its first
	// 24 words, 0.3 s after its start; the data ends 0.82 s after it).
	void p3_bk_rx(int64_t start, const char *what)
	{
		const int kind = what[0] == 'B' && what[1] == '1' ? 0 : what[0] == 'A' ? 1 : 2;
		schedule(start + samples(0.86), EV_BK_DATA, start, kind);
	}
	// Break-in quality (fix 42; item 13, 2026-11-21): how cleanly a break-in or the callee's A2 came
	// through decides the letter we answer it with, and so the new sender's level, as the reference
	// unit's answers do: T (SL5) on a clean one, S (SL3) on a noisy one, else the alternation letter
	// (SL1). Our measure, bk_error(): the mean phase error of the burst's 80 steps (24 pattern + 56
	// data, both carriers), in degrees off the nearest quadrant point after the common offset,
	// demodulated at the carrier timing the break-in decoded with (cc), plus kBkImbDeg degrees per dB
	// of level difference between the two carriers (a burst with one carrier weaker is answered lower
	// than its phase errors alone say; CCIR good: correlation 0.75). T below kBkErrT, S below kBkErrS.
	// Checked on the reference unit's own break-ins in emulator pair runs: white noise clean to 0 dB
	// SNR (180 bursts), CCIR good / moderate / poor at 15-30 dB (200), 7 real RMS break-ins.
	// The first version weighed each step by its pulses' amplitude against the burst's median, so a
	// carrier in a fade (the carriers are 840 Hz apart) counted as error, and found the timing again
	// from each carrier's first strong pulse, so a carrier that began in a fade was placed tens of
	// pulses late: under fading it answered S and P where the reference unit answers T. Without cc
	// the timing is found as before and only steps whose pulses are both above a quarter of their
	// carrier's median count.
	static constexpr double kBkImbDeg = 0.185, kBkErrT = 1.94, kBkErrS = 19.3;
	double bk_error(int64_t start, const double *cc = nullptr) const;
	void p3_bk_decode(int64_t start, int kind);
	// Our break-in burst: the callee's B1 carries the first characters of its
	// greeting, the caller's A2 and every changeover break-in the first ones
	// of our queued text (pending_used_, consumed when the break-in is
	// acknowledged). Built once and repeated unchanged until then.
	const uint8_t *my_bk(bool b1);
	int bk_spoil_n_ = 0;                            // test: P3_BK_SPOIL break-ins spoilt so far
	// Returns where the burst goes (pulse 0): `at`, or later for a late burst (fix 257, queue_psk).
	int64_t queue_words(int64_t at, const uint8_t *w, int n);
	// Fix 255: our A1 (a level-1 packet) carries the level-1 header of the P1 header toggle. The
	// callee checks A1's header against the header picked by the P1 header it expects next: the
	// toggle of our last P1 packet's header. That is 0xAA normally, so A1's fixed header; 0x55
	// after a speed-down of our set-up packet (fix 82), so the other one. With the fixed header
	// there the reference unit read our A1 as carrier-swapped and took nothing: it repeated its
	// P3-accept for many cycles. P3_A1_HDR_OLD=1: the fixed header always (old).
	std::array<uint8_t, 82> setup_a1() const
	{
		std::array<uint8_t, 82> w;
		std::copy(p3::kSetupA1, p3::kSetupA1 + 82, w.begin());
		static const bool old = std::getenv("P3_A1_HDR_OLD") != nullptr;
		if ((p1_hdr_ & 1) && !old) std::copy(p2::kHeader[1][1] + 1, p2::kHeader[1][1] + 9, w.begin() + 1);
		return w;
	}
	void start_p3_master(int64_t ref);
	void start_p3_slave(int64_t a1_slot)
	{
		p3_ = true; p3_first_tx_ = true; level_ = 1; long_ = false; psk_swap_ = false;
		master_ = false; state_ = ST_P3SETUP; misses_ = 0; p3_drift_reset();
		link_status(true, " (P3)");
		log("P3 link set up with " + remote_);
		schedule(a1_slot + samples(0.86), EV_P3, a1_slot, 10);
	}
	// The other side broke in (its burst starts at `start`; our last own burst
	// went out at my_start_): answer with the level letter, receive from now.
	// We become the sender (again): forget the previous sending turn's
	// pending requests. A stale after-S state (p3_up_fails_ >= 0) from an
	// earlier turn made us refuse KB5LZK's R (ack + long) after our next
	// break-in, for ever (30 m, 2026-10-15 18:58; fix 47).
	void new_send_turn();
	// We answered a break-in with `letter`: as after our S, the reference unit
	// as receiver answers the first good packet with Q, repeats T/S on
	// a failure, and after 2 failures sends P and expects one level
	// lower (SL4 after T, SL2 after S).
	// Without it our T's after the answer read as repeats and the sender
	// stayed at SL5 (KC0TPS, 30 m, 2026-10-15 18:47; fix 46).
	void arm_bk_answer(int letter);
	// Fix 83: we answered a break-in starting at `bk` with Q at `q_at`: the
	// breaker sends it again one cycle later. Drop the receive looks scheduled
	// for its first packet and look for the repeat instead.
	std::future<RxPacket> bkd_fut_;
	int64_t bkd_slot_ = -1, bk_q_at_ = -1;
	int bk_rxfail_n_ = 0;                           // test: P3_BK_RX_FAIL decodes failed so far
	void bk_wait_q(int64_t bk, int64_t q_at);
	void on_bk_again(int64_t exp);
	// Fix 270 (TODO ff, fifth on-air session u5-4 / u5-5): the peer repeats its break-in when it did not take our
	// answer. [measured, recordings] A peer break-in read weakly at the tracked timing (soft 0.64-0.69) put our
	// answer 921-926 ms after the burst's onset (the reference unit takes them at 937-943 ms) and our receive
	// lattice ~16 ms early. u5-5: the unit sent the same break-in (data 7a 69 78) again every cycle for ~10 s,
	// 6 of the copies readable at their own timing by the hard look, until it took one of our early answers;
	// u5-4: 2 repeats (data 2c 76 65; the second readable), then it took our third early answer and sent SL3
	// packets on the break-in's own lattice for 30 s, every one decodable offline at that timing, while our
	// looks, 15-20 ms early with a 2 ms search, read "hdr 0" every cycle and the link was lost. We never
	// noticed the repeats. Now, after taking a peer break-in and until a packet decodes, a failed data look
	// also looks for a break-in at the burst (the hard look: its own timing, untracked); one found within
	// kBkRepWin of where the repeat is due is the peer's break-in again: taken at its own timing (our answer
	// at its instant, the lattice re-anchored), its data not delivered a second time. The reference unit does
	// the same as the station taking a break-in: it answers each repeat of our break-in again (emulator:
	// T T P ...). P3_BK_REPEAT_OLD=1: as before (the repeats not looked for).
	static bool bk_rep_old() { static const bool old = std::getenv("P3_BK_REPEAT_OLD") != nullptr; return old; }
	static constexpr double kBkRepWin = 0.035;       // s: the repeat's start this far from where we expect it
	static constexpr int kBkRepMax = 40;            // re-takes after one break-in (the peer's own sender timeout ends it first)
	bool bk_rep_watch_ = false;                     // a peer break-in taken, no packet decoded since
	int64_t bk_rep_first_ = 0, bk_rep_at0_ = 0;     // where we took it, our answer to it (the caller's answer lattice)
	int bk_rep_n_ = 0;                              // re-takes since
	bool bk_rep_dedup_ = false;                     // the data of the break-in being re-taken was delivered already
	std::vector<uint8_t> bk_last_info_;             // the last break-in data delivered
	// A failed data look while bk_rep_watch_: is the burst the peer's break-in again? If so, re-take it.
	bool p3_bk_repeat(int64_t exp);
	void take_p3_bk(double start, int64_t at_override = -1, bool repeat = false);
	// Set-up steps. Caller: 0 = after A1 (expect B1), 1 = after CS T (expect
	// the callee's P3 packet), 2 = after A2 (expect CS T). Callee: 10 = A1
	// due, 11 = after B1 (expect CS T), 12 = after our P3 packet (expect A2).
	// Set-up step 1's first look (fix 79): just after the decode's launch in
	// real time, 0.92 s otherwise.
	double p3_su_look() const { return cfg_.realtime && !std::getenv("P3_LATE_LOOK") ? kP3Launch + 0.004 : 0.92; }
	void on_p3(int64_t ref, int step);

	// ---- receiving ---------------------------------------------------------
	// Received data field -> text. `status` = the packet's plain status byte
	// (bits 2-4 = data type, P2/P3 specs: 0 ASCII, 1 Huffman, 2 Huffman
	// with upper/lower case swapped, 4-7 PMC German/English normal/swapped;
	// the Huffman table is P1's); -1 = ASCII (break-ins).
	// Fix 172: a packet whose acknowledgement the sender missed, sent again repacked at another
	// level. The sender then starts again from that packet's first character: its first packet
	// at the new level (the same counter) holds a prefix of the text we already have (to a lower
	// level) or that text and more (to a higher one), and the next packets go on from there. Fix
	// 84 caught this only when the packed bytes began alike; with another data type (Huffman
	// after PMC) the text was delivered twice (emulator: our answers 8 ms early, the sender
	// stepped from L3 to L2 and resent 55 characters). Now the decoded text is compared: a
	// same-counter packet at another level whose text and the last packet's are one a prefix of
	// the other (4 characters at least) starts a resend, and every following character that
	// matches what the last packet carried is dropped, until one differs or that text is used
	// up. P2_NO_REPACK_DEDUP=1: off.
	bool repack_cand_ = false;
	std::string last_dec_, resend_;
	void dedup_repack(std::string &dec);
	// Fix 203: the first packet of such a resend at another level, taken by fix 84 for the last
	// packet (same counter, its first bytes alike: not new) and so never shown to the check above:
	// the resend was not noticed and the next packets, which carry on from it, printed again what
	// we had (real reference unit, rb2-5: L2 "dia delta alph" printed, its acknowledgement lost, our T; the
	// L1 resend "dia d" taken as a repeat, then "elta " and "alph" printed twice). Now its text goes
	// through the check (only when it agrees with the last packet's, so a coincidence of bytes
	// prints nothing): the resend starts there, and text beyond what we have (a resend at a higher
	// level) is delivered. P2_NO_REPACK_DEDUP=1: off, as fix 172.
	void repack_seen(const uint8_t *info, int nbytes, int status, int lvkey);
	void deliver(const std::vector<uint8_t> &info, int nbytes, int status = -1) { deliver(info.data(), nbytes, status); }
	void deliver(const uint8_t *info, int nbytes, int status = -1);
	// Mean power of the received samples in [a, b) (absolute sample times).
	// The peer's frequency offset from its P1 FSK (call, set-up packet,
	// CS replies), Hz as we hear it: the shift of the tone pair that holds the
	// most energy, +-90 Hz (Hann window, coarse 2 Hz then 0.25 Hz). PSK alone
	// measures the offset only modulo a quarter turn per step (data headers,
	// 25 Hz) or half a turn (control signals, 50 Hz), and the AFC took the
	// alias nearest 0 at link start: links beyond +-25 Hz failed, the P3 callee
	// read the caller's S as R from 30 Hz (emulated firmware; the firmware
	// links with itself at 60 Hz). The FSK tones, 200 Hz apart, are not
	// ambiguous. Returns false when the spectrum has no clear peak.
	bool fsk_offset_hz(int64_t a, int64_t b, double &hz) const;
	double fsk_afc_sum_ = 0, fsk_afc_w_ = 0;
	static bool fsk_afc_on() { static const bool on = std::getenv("P2_NO_FSK_AFC") == nullptr; return on; }
	void fsk_afc_reset() { if (fsk_afc_on()) afc_ = 0; fsk_afc_sum_ = fsk_afc_w_ = 0; fsk_afc_n_ = 0; tx_df_ = 0; p1_df_ = 0; tx_est_ = 0; tx_est_n_ = 0; tx_est_out_ = 0; }
	int fsk_afc_n_ = 0;                              // fix 207: measurements in fsk_afc_sum_
	int p1_trk_n_ = 0; double p1_trk_sum_ = 0, p1_trk_lo_ = 1e9, p1_trk_hi_ = -1e9;   // fix 207: clear replies to our call / set-up (samples after our first bit)
	// Fix 185: the offset the P1 FSK of the set-up is read at (bit_soft, p1_tone_snr_db), Hz as we
	// hear the peer: the grid point the call or reply was found at, then the FSK measurement.
	double p1_df_ = 0;
	// Fix 145: our transmit moved by the peer's offset as measured from its P1 FSK, so that it
	// hears us on its own frequency. The reference unit as sender moves its own transmit to cancel
	// the offset it hears on our control signals, and from 50 Hz on 100 Bd DPSK moves the wrong
	// way (60 Hz taken as -40, the alias): 310 characters in 100 s at 50-70 Hz
	// against 4,389 at 0 Hz; with the offset only on what we hear, 4,389 [bb-ext, emulator].
	// Station pairs stay consistent: the callee measures the call (sent unshifted) and shifts
	// what it sends from then on, its P1 replies too, so the caller measures ~0 from them.
	double tx_df_ = 0;                               // Hz added to everything we send (P2/P3 station bursts)
	static bool tx_afc_on() { static const bool on = std::getenv("P2_NO_TX_AFC") == nullptr; return on; }
	// Only where the reference unit's own correction fails (from 50 Hz; 40 Hz still works without):
	// on the RF bench (radios 13.75 Hz apart) the real reference unit, hearing our shifted signal, kept
	// moving its own transmit ~1 Hz a cycle through 0 and beyond until the link timed out (its
	// view of us is not the mirror of ours there: an asymmetric channel). P2_TX_AFC_MIN_HZ.
	static double tx_afc_min() { static const double v = std::getenv("P2_TX_AFC_MIN_HZ") ? std::atof(std::getenv("P2_TX_AFC_MIN_HZ")) : 40.0; return v; }
	// Fix 223: as CALLER, our transmit follows the callee below 40 Hz too, as the reference unit's own
	// caller does: as the link's caller it moves its transmit toward the callee a little each cycle,
	// as callee it never moves its transmit in the link. Emulator run, the radios 15 Hz apart in one
	// direction (2026-11-12): the reference unit as caller moved its transmit whether it sent or
	// received, ~0.44 Hz a cycle at first, 10 Hz of the 15 in ~60 s; as callee its transmit stayed.
	// So the callee's frequency is the link's reference and the caller follows it.
	// We do the same, only at once: the shift is the callee's offset as we measure it (the set-up's FSK,
	// then every AFC update from its control signals and packets, averaged), clamped to +-50 Hz, and a
	// burst keeps the shift it started with. As callee we keep our transmit where it is: a reference unit caller
	// moves its own, and two stations both moving their transmit to each other drift together on a
	// radio pair that is not symmetric (fix 145's bench run: through 0 and beyond, link lost).
	// P2_NO_TX_FOLLOW=1: off (fix 145's 40 Hz rule only).
	static bool tx_follow_on() { static const bool on = std::getenv("P2_NO_TX_FOLLOW") == nullptr; return on; }
	static constexpr double kTxFollowMaxHz = 50.0;
	double tx_est_ = 0;                              // the peer's offset as we hear it, averaged (Hz)
	int tx_est_n_ = 0, tx_est_out_ = 0;
	void tx_est_feed(double hz, const char *what);
	// Fix 246: the follow moves our transmit a step a cycle toward the target instead of to the
	// measured offset at once (fix 223's jump), as the reference unit's caller moves its own tones:
	// gradually, over many cycles. P2 and P3 links only. The step (track_step_hz): none within the
	// dead band (P3 1.5 Hz, P2 6 Hz), else a share of the error (P3 5 %, P2 2.5 %), at least 0.15 Hz,
	// at most 3 Hz. Our error is the callee's offset as we measure it (tx_est_, clamped to +-50 Hz)
	// less the shift we send now. The set-up's FSK still seeds the shift at once (fsk_afc_add, as
	// fix 223), and fix 145's rule from 40 Hz still jumps.
	// A frequency step (0.3.4's re-acquire) then moves our transmit over several cycles, so the
	// callee, whose receive shift has just been re-measured, follows a drift instead of a second step.
	// P2_TX_FOLLOW_OLD=1: fix 223's jump.
	static bool tx_follow_old() { static const bool on = std::getenv("P2_TX_FOLLOW_OLD") != nullptr; return on; }
	static double track_step_hz(double err_hz, bool p3);
	int64_t tx_step_at_ = -(int64_t(1) << 40);       // fix 246: the last step (one a cycle)
	// Is the follow (not fix 145's rule, not the callee's 0) what tx_target() returns now?
	bool tx_target_follows() const
	{
		if (!tx_afc_on() || !master_ || !tx_follow_on() || tx_est_n_ == 0) return false;
		const double fsk = fsk_afc_w_ > 0 ? fsk_afc_sum_ / fsk_afc_w_ : 0.0;
		return std::fabs(fsk) < tx_afc_min();
	}
	// The shift for the next burst we start, chosen as it starts (fix 223) and moved a step a cycle
	// toward tx_target() while that is the follow (fix 246).
	void tx_shift_choose();
	// The shift the peer hears us moved by now (the 217b gate): fix 223 counted the target.
	double tx_shift_now() const { return tx_follow_old() ? tx_target() : tx_df_; }
	// The shift for the next burst we start (Hz, added to our tones).
	double tx_target() const;
	// One FSK measurement over [a, b): the AFC starts from the duration-weighted mean.
	void fsk_afc_add(int64_t a, int64_t b, const char *what);
	double window_power(int64_t a, int64_t b) const
	{
		double e = 0; int64_t n = 0;
		for (int64_t t = std::max<int64_t>(0, a); t < b && t < n_; t++) { if (n_ - t >= kRing - 1) continue; const double x = raw_[size_t(t % kRing)]; e += x * x; n++; }
		return n ? e / double(n) : 0.0;
	}
	// P2 callee before the first good packet: the slot at `exp` held a burst
	// still on at +0.86 s, the caller's P1 set-up packet again (its P2-accept
	// lost)? Decoded once it is in (+0.955 s); then P2-accept again and the PSK
	// phase from it; else this cycle goes unanswered (the caller repeats).
	void on_p1_again(int64_t exp);
	// Fix 218: P2 callee, no good packet yet in the slot at `exp` (the caller's PSK packet start):
	// P2-accept again at the reply time of the set-up packet the slot would hold (its first bit
	// 13.5 ms before the slot), as the reference unit does (reference-unit pairs, replies cut,
	// 2026-11-11), and look at the next slot.
	void p2_accept_again(int64_t exp, const char *why)
	{
		log(std::string("P2: ") + why + ": P2-accept again");
		queue_accept(exp - samples(0.0135) + samples(kP1Reply - cfg_.rt_latency_s + test_call_answer_s()), p2arq::kP2Accept);
		const int64_t next = exp + samples(cyc_s());
		schedule(next + samples(0.86), EV_PACKET, next);
	}
	void enter_psk_irs(int64_t expected_start);
	// IRS: reply to the packet that started at `start` (CS or break-in).
	// Fix 84: new or repeat. The status counter alone failed after the sender
	// stepped down: emulator, noise 3500 on our receiver, a good SL4 packet
	// (counter 2) acknowledged, failures, our T down to SL1; the sender's SL1
	// packet carried new data with counter 2 again, we took it for a repeat
	// and answered T to it 101 times. Same counter at the same level (long counts as
	// its own level): a repeat. Same counter at another level: new unless its
	// data starts as the last packet we took (the sender re-sending, at the
	// lower level, a packet whose acknowledgement it missed).
	int p3_last_lv_ = -1;
	std::vector<uint8_t> p3_last_data_;
	bool p3_fresh(int cnt, int lv, const uint8_t *d, int n) const
	{
		if (cnt != last_rx_counter_) return true;
		if (std::getenv("P3_COUNTER_ONLY") || lv == p3_last_lv_ || p3_last_lv_ < 0) return false;
		const int m = std::min({n, int(p3_last_data_.size()), 8});
		return m > 0 && std::memcmp(d, p3_last_data_.data(), size_t(m)) != 0;
	}
	void p3_note_rx(int lv, const uint8_t *d, int n) { p3_last_lv_ = lv; p3_last_data_.assign(d, d + std::min(n, 8)); }
	// ---- P3 long packets as receiver (2026-10-17) ----------------------------
	// Fix 173: the long-packet request (--p3-long, off by default) only on a clean path: the
	// smoothed residual of the demodulator's fit (noise / signal; the reference firmware's SL6 packets
	// read 0.29 clean, 0.35 / 0.42 / 0.49 at noise 300 / 500 / 700) at most P3_LONG_RESID (0.36) over
	// at least 4 packets at this level. Long SL6 packets need ~5 dB more than short ones (p3long_test,
	// sl1_sens), and a long packet lost costs 3.75 s.
	double p3_resid_ = 1; int p3_resid_n_ = 0;
	bool p3_long_clean() const
	{
		static const double mx = std::getenv("P3_LONG_RESID") ? std::atof(std::getenv("P3_LONG_RESID")) : 0.36;
		return p3_resid_n_ >= 4 && p3_resid_ <= mx;
	}
	bool p3_rx_long_ = false, p3_r_pending_ = false;
	int p3_long_good_ = 0;
	struct P3Long { bool ok = false; int level = 0; std::vector<uint8_t> data; uint8_t status = 0; double perr = -1;   // perr: fix 229
		double bias = 0;                                                         // fix 241: the common phase step removed (1/16 turn)
		int mq_level = 0, marq = 0; std::vector<std::complex<double>> d0, d1; };   // memory ARQ (long): this copy's products, both maps
	// Memory ARQ for long packets (2026-10-18, as fix 86 for short ones): the
	// per-word differential products z_p conj(z_(p-1)) of failed copies at the
	// expected level, for both carrier maps; on the next failure they are summed
	// with the new copy's (the map alternates with every transmission) and the
	// sum decoded; the expected counter must match. P3_NO_MARQ turns it off.
	struct MqLong { int level; std::vector<std::complex<double>> d0, d1; };
	std::vector<MqLong> mql_;
	static constexpr size_t kMqlMax = 4;   // stored copies (with the newest: 5; SL3 at noise 2500 needed 4-5 offline)
	std::future<P3Long> p3l_fut_;
	int64_t p3l_slot_ = -1;
	static constexpr double kP3LongIn = 3.325;       // a long packet's last pulse is in (330 pulses from slot + 0.8 T)
	static constexpr int kP3LongAfter = 4;           // good new packets in a row before we ask for long
	// Fix 219: P3 receive follows a drifting sample clock. On the bench Pi the sound card's capture
	// now and then runs ~1,000-2,500 ppm slow for 10-30 s (f216b-1: 18 ms in 15 s; sb208b-16: 11 ms in
	// 5 s): the sender's packets then come up to ~3 ms per cycle earlier on our clock. Our next look sat
	// at the last decoded slot + one cycle, searched +-1.5 ms after a decode and +-2 ms after a failure:
	// a drift past that lost every packet from then on (f216b-1: SL5 decodes failed from 88 s, the
	// next one, SL1 at -15.3 ms, 30 s later). This is receive
	// side only: where we look. Our answers stay on the slot we decode (reply_as_irs(p3_slot(r))),
	// so they keep their place against the sender's packet whatever our clock did; an audio loop
	// that holds its round trip (fix 219) keeps them there on the air.
	//  - Drift: from the slot errors of decoded packets (per cycle: error / cycles since the last
	//    decode). Two decodes in a row 1 ms or more off the same way, or one 3 ms or more off after
	//    a miss, start it; then each decode moves it by half its error per cycle (at most 4 ms per
	//    cycle); it ends once it is under 0.3 ms with three decodes in a row within 0.5 ms. While on,
	//    the next look is one cycle plus the drift on, and searched +-2 ms after a decode as well.
	//  - While the audio layer reports our round trip moving (note_audio_slip, within 30 s; the slip
	//    itself, and the moment it hides packets, look like noise from here), the search after
	//    failures widens: +-2 ms after one (as before), then 5 ms more per further
	//    miss (faster than the fastest slip seen, ~3 ms per cycle), at most +-30 ms (the largest slips
	//    seen, 24-28 ms); beyond +-4 ms the sync searches step coarser, then refine (about the same work).
	//    From two misses on, the level last decoded is tried as well (p3_levels_now).
	// P3_DRIFT_OFF=1: none of this (as before). (Tested with a slip test channel.)
	double p3_drift_ = 0, p3_drift_prev_ = 0;        // samples per cycle; the last per-cycle error
	bool p3_drift_on_ = false; int p3_drift_quiet_ = 0;
	static constexpr int kP3SearchMaxUs = 30000;     // the bench's largest slips: 24-28 ms (rb9-2, sb208b-16)
	static constexpr double kSlipHintS = 30.0;       // how long the audio layer's word (note_audio_slip) counts
	int64_t slip_hint_n_ = -1;
	static bool p3_drift_off() { static const bool v = std::getenv("P3_DRIFT_OFF") != nullptr; return v; }
	void p3_drift_reset() { p3_drift_ = 0; p3_drift_prev_ = 0; p3_drift_on_ = false; p3_drift_quiet_ = 0; }
	int64_t p3_drift_step() const { return p3_drift_on_ ? int64_t(std::llround(p3_drift_)) : 0; }
	int p3_search_us(bool ok) const;
	void p3_drift_note(double err, int cycles);
	// The looks for the packet starting at `next`: long, short, or both while
	// a switch to long is pending (the sender may not take our R).
	void schedule_p3_rx(int64_t next, bool ok);
	// The link watchdog (2026-11-16). Every P2/P3 link state goes on by events it schedules for itself
	// (a look at the next packet, the read of our packet's answer, a break-in's answer). A path that
	// returns without scheduling the next one leaves the station silent for good: no answer, no
	// failure counted, so no timeout either (the P3 receiver after a failed packet at the switch back
	// to short packets did exactly that: bench runs cv-1 and cv-4). The reference unit's receiver
	// answers every cycle and drops the link after MAXErr cycles with nothing heard. So: a link up
	// with no such event pending and nothing to send for longer than a cycle (+0.5 s) is stalled.
	//  - P3 receiver: the looks are made again at the next slot on the sender's grid (one short cycle
	//    on from the last slot scheduled), the cycles lost counted as failed packets; the looks then
	//    answer (repeat requests) and count as usual.
	//  - Any other state: one failed cycle counted per cycle stalled, so the link ends by the timeout
	//    (MAXErr cycles, as the reference unit).
	// Logged ("link stalled"); it should never fire. P2_WATCHDOG_OFF=1: none of this.
	int64_t watch_since_ = -1, watch_p3_next_ = -1;
	int watch_fires_ = 0;
	void link_watch();
	void drop_long_looks(int64_t slot)
	{
		events_.erase(std::remove_if(events_.begin(), events_.end(), [&](const Event &e) {
			return e.ref == slot && (e.type == EV_P3L_LAUNCH || e.type == EV_P3L_SYNC || e.type == EV_P3L_PRE || (e.type == EV_PACKET && (e.arg & 1)));
		}), events_.end());
	}
	// SL2-6: sync on the first 12 pulses (the same layout as a short packet,
	// no per-level offset for long packets), then the long layout and
	// long_decode; SL1: the P2 level-1 long packet.
	// Products of one long copy (layout_long word order) for map false (d0)
	// and, if `both`, map true (d1). Wide levels: one demodulation serves
	// both maps; narrow ones demodulate per map.
	static void long_products(int lv, const std::vector<float> &x, double fs, double t, bool both, bool first_odd,
	                          std::vector<std::complex<double>> &d0, std::vector<std::complex<double>> &d1);
	// Fix 236: long_products for a wide level from a plan made early (the same demodulation, both maps).
	static void long_products_planned(const p3::FlatPlan &P, const std::vector<float> &x,
	                                  std::vector<std::complex<double>> &d0, std::vector<std::complex<double>> &d1, p3::FlatProgress *prog = nullptr);
	// A long packet from summed products: phase steps, each word's soft bits
	// weighted by its product's magnitude (normalised; 2026-10-18).
	static p3::LongRx long_decode_d(int lv, const std::vector<std::complex<double>> &d);
	// Fix 229: a long packet's quality from its carriers' phase steps alone: the rms distance of each data
	// word's step from the nearest point (SL2/3: 2 and 10, SL4-6: 2, 6, 10, 14, in 1/16 turns), header
	// words skipped. 0 = perfect.
	static double long_phase_err(int lv, const std::vector<std::complex<double>> &d);
	// Fix 241: a long packet's common phase step. Short packets have always had theirs removed (the
	// header's bias, finish_at: the receive shift's residual, b / (16 T) Hz); long ones never did, so
	// a shift that went stale during long packets rotated every step: on the bench (batch lrt,
	// 2026-11-14) the long SL4 phase-error reading grew packet by packet over the link, 0.74 -> 1.22,
	// the same in two runs 40 minutes apart (the far radio drifting while it sends long packets
	// [inf]; nothing updated the shift on long packets), and stayed above the S limit (0.77), so the
	// link never left long SL4. The bias is the circular mean of the steps raised to the
	// constellation's order (SL2/3: 2 points, SL4-6: 4; header words left out, as long_phase_err),
	// weighted by magnitude, and each product is rotated by it before the decode, the memory ARQ
	// store and the phase-error reading. P3_LONG_BIAS_OFF=1: as before.
	static double long_debias(int lv, std::vector<std::complex<double>> &d);
	// Fix 236: the long decode per level. `pre`: the level's timing searched early (on the packet's first
	// 0.36 s, EV_P3L_SYNC) and, for the wide levels, the plan of the long demodulation made with it; null:
	// searched here, as before. Narrow levels (SL2) demodulate the sync's map first and the other map only
	// when memory ARQ needs it (before: both, always, for the expected level; the same products).
	// prog: the demodulation's right-hand side and forward substitution made while the packet arrived
	// (advance_p3_long), finished at the decode; the same operations, so the same products.
	struct P3LProg { std::mutex m; p3::FlatProgress g; };
	struct P3LSync { p3::LvSync s; std::shared_ptr<const p3::FlatPlan> plan; std::shared_ptr<P3LProg> prog; };
	P3Long decode_p3_long_lv(int64_t slot, const std::vector<float> &x, int64_t a, int lv, const Snap &sp,
	                         int mq_lv, int mq_cnt, const std::vector<MqLong> *mq_store, const P3LSync *pre);
	// The levels in order, as one decode tried them in turn: the first that decodes; memory ARQ products
	// from the level that made them (the expected level, first).
	static P3Long p3_long_pick(std::vector<P3Long> &rs);
	// Fix 236: the long receive budget. A long packet's last pulse is in at slot + 3.325 s and our answer
	// is due ~3.379 s: ~50 ms, where the decode took a timing search (+-3 ms: 50-110 ms per level on the
	// build VM, single core) and a demodulation plan (30-57 ms) before the demodulation and the Viterbi
	// (5-20 ms + 1-8 ms), and a second level only after the first failed. Now the search and the plan run
	// at slot + 0.30 s (both need only the first 12 pulses and the window's length), one task per level,
	// and at slot + 3.325 s the levels decode in parallel, the first in order taken: the same decode.
	// P3_LONG_EARLY_OFF=1: as before.
	struct P3LEarly { int64_t slot = -1; double df = 0; std::vector<std::pair<int, std::shared_future<P3LSync>>> f; std::vector<std::pair<int, std::shared_ptr<P3LProg>>> prog; };
	P3LEarly p3l_early_;
	std::vector<int> p3_long_levels(int64_t slot, bool *trimmed = nullptr) const;
	static bool p3_long_early_off() { static const bool v = std::getenv("P3_LONG_EARLY_OFF") != nullptr; return v; }
	void launch_p3_long_sync(int64_t slot);
	// The long demodulation in pieces while the packet arrives (wide levels, with the early plan): at
	// slot + kP3LPre[i], the right-hand side of every pulse already in and the forward substitution as
	// far as it goes, so at the deadline only the last pulses, the back substitution and the Viterbi are
	// left (on the Raspberry Pi 4 the long SL6 demodulation was 21 ms of a 48 ms decode, 13 of it the
	// right-hand side). The same operations, so the same products (p3::demod_flat_advance). The audio up to now, the same window start, the same receive shift as the
	// early search (else nothing is used); the last 64 samples left out (the receive shift's filter,
	// 63 taps either side, reads past them). P3_LONG_PRE_OFF=1: as before (all at the deadline).
	// (not at 0.80-0.95 s: the packet's short look, kP3Launch, decodes then)
	static constexpr double kP3LPre[6] = {1.0, 1.5, 2.0, 2.5, 2.95, 3.2};
	static bool p3_long_pre_off() { static const bool v = std::getenv("P3_LONG_PRE_OFF") != nullptr; return v; }
	void advance_p3_long(int64_t slot);
	void launch_p3_long(int64_t slot);
	// Fix 236: the long-packet decode budget (real time). A long decode that is late, or done with less
	// than kThinMs to spare, sends us back to short packets (R) and we ask for long ones again only after
	// a hold: 120 s, doubled for each hold of the link (at most 8x). The level is not capped for it (the
	// short decodes are what the level cap is for; before, a late long decode capped the level).
	// P3_LONG_BUDGET_OFF=1: as before.
	int64_t p3l_hold_ = 0;                           // no long requests before this sample
	int p3l_holds_ = 0;                              // holds this link
	static bool p3_long_budget_off() { static const bool v = std::getenv("P3_LONG_BUDGET_OFF") != nullptr; return v; }
	bool p3_long_budget_ok() const { return !cfg_.realtime || p3_long_budget_off() || n_ >= p3l_hold_; }
	void p3_long_hold(const std::string &why)
	{
		if (!cfg_.backoff) return;
		const double h = kCapHoldS * double(1 << std::min(3, p3l_holds_++));
		p3l_hold_ = n_ + samples(h);
		log("P3: long-packet budget: " + why + ", short packets only for " + std::to_string(int(h)) + " s at least");
	}
	// Fix 241: long packets as receiver in real time, adaptive (owner, 2026-11-13: "if the cpu cannot do
	// sl6, I'd like for it to automatically be disabled, but otherwise used"). Replaces fix 217b's gate
	// (no long packets in real time unless P3_LONG_RT=1) and, by default, fix 236's timed hold.
	// A ceiling: we ask for long packets (R) only at a level at or below it. Every long decode is
	// measured against its answer deadline (fix 236's margin): a late one at SLn lowers the ceiling to
	// n - 1, one done with less than kThinMs to spare to n (the next level's decode is longer: it
	// would be late). Above the ceiling we ask for short packets (R) and climb on them; at the ceiling
	// a speed-up due goes the same way (R first, then S on short packets: short SL n+1 carries more
	// than long SL n), and R is not asked again at or above the ceiling until the link falls below it,
	// so nothing flaps. The ceiling belongs to the computer, not the link: it is kept from link to
	// link, so a slow computer pays one repeated long packet per level it cannot do, once. It is
	// raised by one level at a link's start only when the last link decoded at least p3l_need_ long
	// packets at the ceiling, all with at least 2 x kThinMs to spare; a raise that fails again doubles
	// that count. No start-up probe: an idle computer's timing says little about the same computer
	// with the audio, the GUI and the other decodes running, and the real decodes are measured anyway.
	// cfg_.p3_request_long false: no long packets at all (the user's "off").
	// P3_LONG_RT=0: never in real time (fix 217b); P3_LONG_RT=1: always, with fix 236's hold (no
	// ceiling); unset: adaptive. Lockstep runs (no deadline) are not affected.
	static int p3_long_rt_env() { static const int v = std::getenv("P3_LONG_RT") ? (std::atoi(std::getenv("P3_LONG_RT")) > 0 ? 1 : 0) : -1; return v; }
	bool p3_long_adaptive() const { return cfg_.realtime && p3_long_rt_env() < 0; }
	int p3l_ceil_ = 6;                               // the highest level we ask long packets at (0: none); kept between links
	int p3l_need_ = 10;                              // long decodes at the ceiling, all with room, that let it rise at the next link
	int p3l_room_n_ = 0;                             // this link: long decodes at the ceiling with >= 2 kThinMs to spare
	bool p3l_room_bad_ = false;                      // this link: one at the ceiling had less
	bool p3l_raised_ = false;                        // the ceiling was raised at this link's start
	bool p3l_up_ = false;                            // this link: left long packets at the ceiling to climb on short ones
	void p3_long_lower(int lv, const std::string &why);
	void p3_long_measured(int lv, double ms);
	void p3_long_link_start();
	void on_p3_long(int64_t exp);
	// Fix 168: the leading carrier of our answer (P2 receiver). Each station's
	// bursts alternate the leading carrier every cycle; the called station's
	// control signal leads with the same carrier as the packet it answers, the
	// caller's with the other one, and a control signal in the other order is
	// refused like a missing one: a sender hearing only that repeats its packet
	// until it times out [bb-emu: the capture generator (a),
	// (b)]; the firmware toggles both orders once per cycle.
	// Our toggle per burst keeps that only while we send exactly one burst
	// per cycle; any cycle with none or two would leave every later answer
	// refused. So each decoded packet sets the order of the answer from its own
	// order and our role. P2_TONE_TOGGLE_ONLY=1: the toggle alone (old).
	// P2_TEST_TONE_SLIP=T: test hook, from T s on our answers in the wrong order.
	// Fix 171: our answers' timing hunted when the sender stops reading them. The emulated
	// firmware reads our answers (as callee) only within about +-5 ms of where it expects them,
	// and near the edge it reads some letters and not others: 8 ms late it reads P and almost
	// never Q, 8 ms early the reverse; then it repeats one packet while we hold it [bb-emu]: the real reference unit's P2 receive stalls look
	// the same (only P read). A packet we already hold, decoded 6 times in a row, moves our answer
	// time through 0, -4, +4, -8, +8, -12, +12 ms, 2 cycles each, our letter alternating every
	// cycle meanwhile (each offset gets both letters: near the edge only one of them is read),
	// until a new packet comes; the offset then stays. P2 receiver only. P2_NO_HUNT=1: off.
	//
	// Fix 205: why the sender stops reading our answers, and the hunt re-centred. The reference unit as
	// P2 sender reads our control signals in a window set at the link's start by our P1 answer to its
	// call, which then follows our answers only slowly. Our answer counts only within about +-5 ms of
	// the window's centre; beyond either edge it reads late P mostly and Q hardly ever
	// (the real reference unit's stall: only P read), and the window then drifts
	// further AWAY from us, so the link stays dead until it times out [bb-emu: the real
	// reference unit's radio-2 recordings of the clean bench's t723-t725 replayed into the emulated
	// firmware; in a live link our P1 answer 6 ms early, P2_NO_HUNT=1: 268 characters].
	// Fix 171's hunt (below) finds the zone again in every such case on the reference unit.
	// P2_HUNT_RECENTRE=1 (bench option, not the default): the first offset that works lies within
	// one step of the edge we were beyond, so once it has carried kConfirm new packets in a row
	// it moves kRecentre further the same way and becomes the base for later hunts. It helps
	// against a later drift (two-step shift +7 / +10 ms: last character 89 s against 107 s) but
	// takes a wrong side when we are early: far early the sender still reads both letters
	// now and then, three in a row at times (P1 answers 5 ms late: 145 s against 104 s).
	// Test hooks: P2_TEST_CALL_ANSWER_MS=X (our P1 answers as callee
	// X ms later: the sender's window X ms later), P2_TEST_CS_SHIFT="T:X[,T:X]" (from T s
	// on our P2 answers X ms later).
	double hunt_s_ = 0, hunt_base_ = 0; int hunt_i_ = 0, deaf_ = 0, confirm_left_ = 0, probation_ = 0;
	static constexpr int kHuntAfter = 6, kHuntEach = 2, kConfirm = 3;
	static constexpr double kRecentre = 0.003;
	// Fix 208: our P2 answers as callee on the time reference of our P1 answer to the call, which is
	// what placed the sender's window: the reply instant less rt_latency_s, as every other callee answer
	// (P1: kP1Reply - rt_latency_s; P3: kP3CalleeReply - rt_latency_s), instead of plus reply_adjust_s.
	// With The self-timing (rt_latency = round trip - 65 ms, reply_adjust = 68 ms - round trip)
	// the two differ by a constant 3 ms: our P2 answers sat 3 ms later against the window than our P1
	// answer had placed it, and on the reference unit (both 0) they are centred (live links, hunt off: clean
	// from -4 to +4 ms, not at +-5). Bench, fix 205/206 builds, 16 P2 receive runs without
	// hooks, the real reference unit's radio-2 audio replayed into the firmware (a bench tool): our answers
	// sat 3.0-5.0 ms late of the window's centre (median 4.0; one 6.5), in 13 of 16 runs with clean reads
	// only 0-1 ms further late; the zone is 8-10 ms wide. In our own output (P2_RXLOG,
	// a bench tool) our P1 answer against the lattice of our P2 answers varied only 83.0-84.1 ms
	// between those runs (the reference unit: 87.1): the 3 ms constant and ~0.7 ms more, and no run-to-run
	// scatter worth carrying over from the call's timing. So: the 3 ms are removed here, and kP2CsBias
	// puts the answers 2 ms earlier still: the bench at ~-1 ms of the centre (-2..0 over those runs),
	// the reference unit at -2 ms; the zone holds +-4.5 ms. Callee break-ins keep their old place (the reply lattice +
	// callee_bk_adjust_s: the bench's break-ins pass there). P2_CS_REF_OLD=1: as before (no shift, no
	// bias); P2_CS_BIAS_MS=X: the bias X ms instead.
	static constexpr double kP2CsBias = -0.002;
	double cs_ref_s() const;
	// The instant of our P2 control signal answering the burst that started at rx_start (callee: on the
	// P1 answer's reference; caller, P3: the reply lattice).
	int64_t cs_at(int64_t rx_start, bool after_long) const { return reply_at(rx_start, after_long) + samples(cs_ref_s()); }
	// When the decode for that answer must be in (3 ms before it goes out); the hunted offset and the
	// test shift included (fix 208; P2_CS_REF_OLD=1: the reply lattice alone, as before).
	int64_t cs_deadline(int64_t rx_start, bool after_long) const
	{
		static const bool old = std::getenv("P2_CS_REF_OLD") != nullptr;
		if (old || p3_) return reply_at(rx_start, after_long) - samples(0.003);
		return cs_at(rx_start, after_long) + samples(hunt_s_) + test_cs_shift() - samples(0.003);
	}
	// Fix 208, the hunt (fix 171) reworked:
	// - Sooner when the stall comes out of a clean link: 4 repeats of a packet we hold start it when, before
	//   that run, our last 8 decodes all succeeded and at most one of our 16 answers before it went to a
	//   repeat; else 8 (fix 171: 6 always). A window stall starts abruptly and lasts; a sender that misses
	//   our letters in noise repeats now and then first (emulator, --tx-noise 12000 with our decodes
	//   clean: runs of 4 came often, and every hunt there cost characters). P2_HUNT_AFTER=N,
	//   P2_HUNT_AFTER_NOISY=N.
	// - Toward the likely side first. The letter the sender must read to move on is the one we
	//   acknowledged the held packet with. Late it reads only P (Q mostly not),
	//   so it stalls with Q due; early the reference firmware reads Q and not P, the real reference unit's
	//   recordings read neither. Q due: early first (-4, +4, -8, +8, -12, +12); P due: late first.
	//   Fix 208b: early first always (sb208-14: P due, late first, wrong); late first only with P due
	//   after p2_up_result has seen us early (requests not taken).
	// - (Fix 208b: three, see hunt_keep.) Two new packets in a row at an offset end the hunt, and that offset becomes the base the next
	//   hunt starts from; one new packet and then the same packet again: on to the next offset. Inside
	//   the zone the sender's window slowly follows our answers, so an offset that works gets
	//   centred by it.
	// - Every offset tried and none kept: back to the base, and the next hunt only after twice the
	//   repeats (up to 4x, reset by 24 new packets). Fix 171 went round its list for ever and kept an
	//   offset after a single new packet "on probation": with the sender missing our letters in noise
	//   alone (emulator, --tx-noise 12000, our decodes clean) both walked us to +-8..+-16 ms, beyond
	//   the zone, for most of the link.
	// - Repeats while our S or R is pending are not counted (fix 170's rule answers them).
	// - Meanwhile the held packet is answered with the letter it needs every cycle, the other letter
	//   every 4th (fix 170's probe), not alternating every cycle (see the IRS answer).
	// P2_HUNT_OLD=1: fix 171's hunt (with P2_HUNT_RECENTRE=1, fix 205's option).
	int hunt_k_ = 0;                                  // the repeats that started this hunt (0: not hunting)
	bool hunt_trial_ = false;                         // a new packet came at the hunted offset; a second keeps it
	int hunt_backoff_ = 1, hunt_quiet_ = 0;           // x the repeats before a hunt (after one that found nothing); new packets since
	int hunt_dir_ = -1;                               // -1: early first, +1: late first
	unsigned fail_hist_ = 0;                          // our last 8 decodes, 1 = failed
	unsigned rep_hist_ = 0;                           // our answers, newest in bit 0: 1 = to a packet we already held
	bool hunt_noisy_ = false;                         // the run of repeats began after repeats or failed decodes
	int ack_new_cs_ = -1;                             // the letter that acknowledged the packet we hold (P/Q; -1: other)
	// Fix 208b: an offset is kept after kHuntKeep new packets in a row at it, one of them moved on by the
	// letter that was due when the hunt began (the one the sender had stopped reading).
	int hunt_new_ = 0, hunt_fail_cs_ = -1; bool hunt_fail_seen_ = false;
	static int hunt_keep() { static const int v = std::getenv("P2_HUNT_KEEP") ? std::max(1, std::atoi(std::getenv("P2_HUNT_KEEP"))) : 3; return v; }
	static bool hunt_due_dir() { static const bool v = std::getenv("P2_HUNT_DUE_DIR") != nullptr; return v; }
	// Fix 208b: our speed-up / long request not taken kUpMissStep times in a row while our P/Q move the
	// sender on is a timing symptom: answers 8+ ms early, the firmware reads P and Q but not S or R
	// (emulator, P1 answer +7 ms: link held at level 2, 2,033 of 4,356; bench sb208-12: 966). Then our
	// answers step 6 ms later (P2_UP_MISS_MS; the base of later hunts too), at most kUpMissMax steps per link,
	// and the requests start again from no backoff. P2_UP_MISS_STEP=0: off.
	int up_miss_run_ = 0, up_miss_steps_ = 0;
	bool early_seen_ = false;                         // requests not taken lately: we were early (a P-due stall then hunts late first)
	static double up_miss_late() { static const double v = std::getenv("P2_UP_MISS_MS") ? std::atof(std::getenv("P2_UP_MISS_MS")) / 1000.0 : 0.006; return v; }
	static int up_miss_step() { static const int v = std::getenv("P2_UP_MISS_STEP") ? std::atoi(std::getenv("P2_UP_MISS_STEP")) : 2; return v; }
	static constexpr int kUpMissMax = 2;
	void p2_up_result(bool taken);
	static int hunt_after_clean() { static const int v = std::getenv("P2_HUNT_AFTER") ? std::max(2, std::atoi(std::getenv("P2_HUNT_AFTER"))) : 4; return v; }
	static int hunt_after_noisy() { static const int v = std::getenv("P2_HUNT_AFTER_NOISY") ? std::max(2, std::atoi(std::getenv("P2_HUNT_AFTER_NOISY"))) : 8; return v; }
	static bool hunt_old() { static const bool v = std::getenv("P2_HUNT_OLD") != nullptr || std::getenv("P2_HUNT_RECENTRE") != nullptr; return v; }
	static double test_call_answer_s() { static const double v = std::getenv("P2_TEST_CALL_ANSWER_MS") ? std::atof(std::getenv("P2_TEST_CALL_ANSWER_MS")) / 1000.0 : 0.0; return v; }
	int64_t test_cs_shift() const;
	bool hunting() const { static const bool off = std::getenv("P2_NO_HUNT") != nullptr; return !off && (hunt_old() ? deaf_ >= kHuntAfter : hunt_k_ > 0); }
	void p2_hunt_update(bool ok, bool new_info);
	int ans_swap_ = -1;
	int tone_slips_ = 0;
	void p2_tone_anchor();
	void reply_as_irs(int64_t start, bool was_long, bool new_info, bool ok);
	// Mean power of the received signal over the last `s` seconds.
	double recent_power(double s) const
	{
		const int64_t k = std::min<int64_t>(n_, samples(s));
		double e = 0;
		for (int64_t t = n_ - k; t < n_; t++) e += double(raw_[size_t(t % kRing)]) * raw_[size_t(t % kRing)];
		return k ? e / double(k) : 0.0;
	}
	// P2 receive. The pulse timing comes from the packet's first 0.3 s
	// (EV_P2_SYNC, on a worker); demodulation and decoding start when the
	// packet is in. Our answer is due only ~20 ms after that (callee), and a
	// long packet's decode is ~30 ms even split across cores, so in real
	// time it runs on a worker and is never waited for on the audio loop.
	static constexpr double kP2Sync = 0.30;
	struct P2Timing { bool ok = false; double c[2] = {0, 0}; };
	std::shared_future<P2Timing> p2_sync_fut_;
	int64_t p2_sync_slot_ = -1;
	int64_t setup_again_exp_ = -1;                   // a burst still on at +0.86 s in this slot (fix 104): the set-up packet again if it does not decode
	std::future<RxPacket> p2_fut_, p2_late_;
	int64_t p2_fut_slot_ = -1, p2_late_slot_ = -1;
	RxPacket late_;                                  // fix 200: a late decode's result, taken when done (p2_late_poll)
	int64_t late_slot_ = -1;                         // its slot; -1: none
	int64_t p2_decided_ = -1;                        // the last slot answered (decoded or not)
	bool p2_late_reuse_ = false;                     // p2_late_ may serve the repeat: its slot was answered with a repeat request
	// Fix 200: a decode that finished late is taken as soon as it is done (each launch and each
	// look polls), so its timing teaches the track (fix 149) before the next decode is launched,
	// and its result waits in late_ for the repeat it caused (fix 53; long packets too).
	void p2_late_poll();
	std::chrono::steady_clock::time_point p2_launch_wall_;
	bool p2_fut_long_ = false;
	bool p2_long_possible() const { return rx_long_ || (p2_up_pending_ && p2_req_cs_ == p2arq::CS_R); }
	static constexpr double kP2Launch = 0.845;       // fix 73: a short packet is in by then
	void p2_launch(int64_t exp, bool waited_long);
	// A long packet's joint fit in pieces (scs-modem TODO o). With one core per station (the Pi 4) the
	// whole fit at the end of the packet, from 2.5 s after the short look, took 31-95 ms against the
	// ~30 ms to our answer: every long packet was late, counted failed, and the link fell to level 1
	// with nothing decoded in time (17 characters in 90 s). At kP2LPre the fit's sums over the samples
	// already in are made on a worker (p2::ls_pulses_pre), for the inputs the decode will
	// use (the track, the early timing, the AFC as now); the decode takes them only if its inputs are
	// exactly these (p2::LsPre), so it computes the same numbers. P2_LONG_PRE_OFF=1: the old way.
	static constexpr double kP2LPre[6] = {1.0, 1.5, 2.0, 2.5, 3.0, 3.25};
	std::shared_ptr<p2::LsPre> p2l_pre_;
	int64_t p2l_pre_slot_ = -1;
	bool p2_long_pre_on() const { static const bool off = std::getenv("P2_LONG_PRE_OFF") != nullptr; return cfg_.realtime && !off && !p3_ && pool_.size() > 0; }
	void advance_p2_long(int64_t exp);
	void launch_p2_sync(int64_t exp);
	void on_packet(int64_t exp, bool waited_long, bool relook = false);

	// What came back after our burst (P2): a break-in (want_bk) or which
	// control signal. One demodulation shared by both tests, the carrier-slip
	// variants (see find_packet) made only when needed: the separate
	// looks_like_bk and find_psk_cs did up to six, 25-35 ms on the audio
	// loop on the RF bench, against a 40 ms playback cushion (underruns).
	// Soft score of a control-signal pattern on 21 steps: the mean cos of the
	// phase error after the pattern's own bias fit (1 = every step exact).
	static double pattern_soft(const std::vector<std::array<double, 2>> &st, const char *pat, double prior);
	// Soft score of the break-in pattern (pulses 1-20, as bk_match) with word 0
	// at z index 1 + o: the mean cos of the phase error after a bias fit.
	double bk_soft(const Demod &d, int o, double prior, double *bias_out = nullptr) const;
	static double bk_soft_min() { static const double v = std::getenv("P2_BK_SOFT") ? std::atof(std::getenv("P2_BK_SOFT")) : 0.60; return v; }
	// ---- control-signal timing tracking (fix 62) ----------------------------
	// As for packets (fix 57): a control signal's own first pulses give its
	// timing badly at low SNR; the peer's replies keep their timing against
	// our expected instant, so each one detected gives the next.
	// Two tracks (fix 72): short-cycle acknowledgements, and long-cycle ones
	// (after our long packets the receiver answers on its own 3.75 s grid; our
	// next start follows it, so its answers keep their timing against ours too,
	// but a different one than in the short cycle).
	struct CsTrk {
		bool ok = false, jump_ok = false, turn_new = false;
		double dt = 0, lead = 0, stag = 0, jump_dt = 0, jump_stag = 0;
		int swap = 0, calls = 0;
		int64_t last_exp = -1;
	};
	CsTrk cstk_[2];
	bool cst_ok_short() const { return cstk_[0].ok; }
	int su_cs_ = -1; int64_t su_exp_ = 0;            // fix 68: the last untracked look's soft letter
	int look_soft_cs_ = -1; double look_soft_ = 0, look_margin_ = 0;   // fix 210: find_psk_cs's last soft reading
	int look_hard_cs_ = -1;                          // fix 231c: its hard reading (look_score_: fix 235)
	int su_fail_ = 0, su_letter_ = p2arq::CS_T;       // fix 69: set-up failures after our answer to B1, the letter we repeat                       // a new sending turn: the track holds but is not trusted to refuse a detection, and the first one moves it
	void cst_clear() { cstk_[0] = CsTrk{}; cstk_[1] = CsTrk{}; }
	CsPred cs_pred(int64_t exp, int k = 0);
	void cs_track(const ReplyRx &r, bool soft);
	ReplyRx classify_reply(int64_t exp, bool want_bk, const Snap *sp, double afc) const { return classify_reply(exp, want_bk, sp, afc, CsPred{}); }
	ReplyRx classify_reply(int64_t exp, bool want_bk, const Snap *sp, double afc, CsPred pred) const;
	// Soft acceptance (fix 62): thresholds from noise-only runs (a bench tool --cs).
	static double cs_soft_min() { static const double v = std::getenv("P2_CS_SOFT") ? std::atof(std::getenv("P2_CS_SOFT")) : 0.50; return v; }
	static double cs_margin_min() { static const double v = std::getenv("P2_CS_MARGIN") ? std::atof(std::getenv("P2_CS_MARGIN")) : 0.25; return v; }
	// With the track locked, a hard detection far from it is taken only as a
	// relock candidate (cs_track), not acted on: the peer's reply timing does
	// not jump, noise does (the hard detector's ~5e-4 false rate per look).
	bool cs_hard_on_track(const ReplyRx &r) const { const CsTrk &T = cstk_[r.trk]; return r.score >= 0.9 || T.turn_new || !r.tracked_look || !T.ok || std::getenv("P2_NO_CSTRACK") || std::fabs(r.start - (double(r.exp) + T.dt)) <= 0.6 * T_; }
	static bool cs_soft_ok(const ReplyRx &r) { return r.soft_tracked && r.soft_cs >= 0 && r.soft >= cs_soft_min() && r.margin >= cs_margin_min() && !std::getenv("P2_NO_CSSOFT"); }
	// The offset a soft-accepted control signal measured: its letter's own bias
	// fit (+-4/16 around the AFC). It returned the prior, so once the link
	// tracked (every acknowledgement soft-accepted) the AFC learnt nothing from
	// control signals again (2026-10-19). P2_CS_SOFT_NOBIAS: the old way.
	double pattern_bias_of(const Demod &, const ReplyRx &r) const { return std::getenv("P2_CS_SOFT_NOBIAS") ? r.bias : r.soft_bias; }
	std::future<ReplyRx> rr_fut_;
	int64_t rr_slot_ = -1;
	// The reply at `exp`, classified on a worker in real time (polled every
	// 2 ms up to `deadline`, then counted as nothing heard). False: not
	// ready yet, on_reply runs again (arg | 8).
	bool reply_rx(int64_t exp, int64_t ref, int arg, bool want_bk, int64_t deadline, ReplyRx &out);
	// Fix 250: P3 looks for a control signal (and a break-in) off the audio loop. They ran in
	// the audio loop (P2's are on a worker, reply_rx): on a Raspberry Pi 4 one took 15-45 ms, and
	// 55-60 ms with the station on one core (its parallel_for helpers then share that core), against
	// ~40 ms of queued playback: a playback underrun, its late audio dropped. Now the same reading
	// (bk_look and classify_reply, from a snapshot) is made on a worker, polled every 2 ms, and what
	// it changes is applied here when it is in. At `deadline` (just before our next burst) the look
	// waits for it, so no answer is lost to a slow computer; nothing is read differently.
	// P3_LOOK_INLINE_OLD=1: in the audio loop, as before.
	struct P3Look { BkLook bk; ReplyRx cs; };
	std::future<P3Look> p3k_fut_;
	int64_t p3k_exp_ = -1; int p3k_ev_ = -1;
	CsPred p3k_pbk_, p3k_pcs_;
	int64_t p3k_trk_exp_ = -1; int p3k_trk_calls_ = 0; int p3k_trk_k_ = 0;
	P3Look p3k_res_;
	bool p3_look_async() const { static const bool old = std::getenv("P3_LOOK_INLINE_OLD") != nullptr; return cfg_.realtime && !old && pool_.size() > 0; }
	// want_bk: a break-in is looked for too (bk_track: at the tracked timing); track: as find_psk_cs.
	// False: not in yet, `ev` (ref, arg) runs again in 2 ms. True: p3k_res_ holds the reading.
	bool p3_look(int64_t exp, bool want_bk, bool bk_track, int track, int64_t deadline, EvType ev, int64_t ref, int arg);
	// After p3_look: a break-in? (as looks_like_bk). A break-in taken skips the control-signal
	// reading, whose timing prediction the inline look never made: its look count is put back.
	bool p3_look_bk(int64_t exp, double &start)
	{
		if (!looks_like_bk_apply(exp, p3k_pbk_, p3k_res_.bk, start)) return false;
		if (p3k_pcs_.trk == p3k_trk_k_) { cstk_[p3k_trk_k_].last_exp = p3k_trk_exp_; cstk_[p3k_trk_k_].calls = p3k_trk_calls_; }
		return true;
	}
	// After p3_look: the control signal (as find_psk_cs)
	// Fix 257: a control signal that began late (a long audio path: the far end answers late, and our
	// own round trip adds to it), whose last steps are not heard yet when the look must decide (before
	// our next packet). classify_reply scores all 21 steps, the unheard ones as misses: 14 of 21
	// heard read "cs - score 0.667" for every letter sent (two of our stations at a 100 ms prime),
	// and the sender repeated its packet for ever. Here the steps heard are scored alone, at least
	// kCsHeardMin of them (the five letters' first 12 steps differ in 6 places or more; all 21 in
	// 12), with at most one step in ten wrong and the next letter at least 4 steps worse. Only with
	// a longer round trip than the default path's (rt_latency > 0), in real time; a look that
	// already read a letter (hard 0.9 or more, or soft) is unchanged. P3_CS_HEARD_OFF=1: off.
	static constexpr int kCsHeardMin = 12;
	bool late_cs_heard() const { static const bool off = std::getenv("P3_CS_HEARD_OFF") != nullptr; return !off && cfg_.realtime && cfg_.rt_latency_s > 0.0f; }
	void heard_cs(int64_t exp, const Snap *sp, double afc, int64_t heard, ReplyRx &r) const;
	int p3_look_cs(int64_t exp, double &start, int track) { return find_psk_cs_from(exp, p3k_res_.cs, start, track); }
	// The last instant a look may finish before our burst at `next` (its leading tail first)
	int64_t p3_look_deadline(int64_t next) const
	{
		const int64_t d = next - int64_t(std::lround(p2::preroll(cfg_.fs))) - samples(0.003);
		return p3_ ? std::min(d, p3_tx_earliest(next) - samples(0.003)) : d;
	}
	// Fix 265 (TODO ff 3): where our next P3 burst at `slot` begins, the earliest of the ones we may send there (every
	// level; our long packet when on): a level 2-4/6 burst begins 30 ms before its pulse 0 (queue_lv),
	// SL5 20 ms (queue_p3), PSK bursts the preroll (12 ms). The looks and their deadlines were placed by the
	// preroll alone: on the air every callee burst at SL2-4 (data, greeting) was "queued 4.5 ms after its start:
	// its first part not sent" and made on demand ("TX audio not ready at its start: waited 6-7 ms", the audio loop
	// blocked), in every run (a.3 22 times; n4a6's greeting at SL4 34 times in a row). P3_LOOK_LEAD_OLD=1: the preroll.
	int64_t p3_tx_earliest(int64_t slot) const;
	// A classified control signal: log it, track the offset (find_psk_cs).
	int reply_cs(const ReplyRx &r);
	// ISS / break-in sender: what came back after our burst at `ref`?
	// arg: kind (0 short, 1 long, 2 our break-in) | 8 (a re-look, reply_rx)
	void on_reply(int64_t ref, int arg);
	// Fix 209: a clean reading of the receiver's control signal, fit to time the follow by:
	// hard score >= 0.95, the soft letter (if any) the same at >= 0.9, the carriers not
	// re-aligned (fix 196). On the bench the readings 14-36 ms off were all below this
	// (rb8-2: score 0.81-0.90, soft 0.46-0.83; rb10-1: score 0.81, soft 0.65), the clean
	// ones within +-2 ms of the receiver's place (score 1.0, soft >= 0.99).
	static bool long_read_good(const ReplyRx &r)
	{
		return r.score >= 0.95 && r.wide == 0 && (r.soft_cs < 0 || (r.soft_cs == r.cs && r.soft >= 0.9));
	}
	// Long cycles, we send: follow the receiver's control signal (fix 209). The reference unit as
	// receiver sends it at a fixed place in its cycle,
	// whatever the level, the letter or the packet length, and that place moves only slowly in a
	// link; it decodes our packets only within a few ms. So the signal never jumps, and a move of ours by
	// more than a ms or so in one cycle loses the link: rb10-1 (2026-11-11), the reference
	// taken from a weak reading 16 ms early (P, score 0.81), the clean readings at the
	// receiver's real place (+16 ms) taken twice for a jump and followed at once: every
	// packet after it was asked for again until the link timed out. Now:
	//  - the reference (where the signal lands relative to our packet) is the first clean
	//    reading (long_read_good; P3 has no quality measure: every reading);
	//  - a clean reading within 4 ms of it steers our next start with a proportional-integral
	//    follow: 0.4 of the difference (P2_LONG_GAIN) plus the drift estimate, which takes 0.08
	//    of each difference (P2_LONG_RATE_GAIN); every move at most 1 ms (P2_LONG_STEP_MS).
	//    Drift is followed without lag (130 ppm is 0.5 ms a cycle) and a single reading never
	//    moves us far. The lag matters: the reference unit's long-packet window is narrow (emulator,
	//    our packets held 1.5 / 3 ms late: 10,125 / 3,371 of 13,146 characters; 1.5 / 3 ms
	//    early: 13,146 / 12,036), and a proportional follow alone sat 1-1.5 ms late at 130 ppm;
	//  - a clean reading further off is never followed: two in a row at the same new place
	//    (within 2 ms) mean the reference was wrong, and it is re-taken there (their mean)
	//    without moving (the receiver's grid does not jump; our timing read can);
	//  - a weak reading only moves us by the drift estimate.
	// P2_LONG_FOLLOW_OLD=1: fix 77's rule (follow up to 4 ms at once, a jump seen twice).
	// Test hook P2_TEST_LONG_CS="K:MS[w][,K:MS[w]]": the K-th long-cycle reading of the link
	// (from 1) taken MS ms later than measured, with w as a weak one (as the bench's).
	// Fix 217: where a P2 control signal begins, from its energy envelope (2.5 ms moving mean of
	// |x|, the first crossing of half-way from the floor before it to the median over its body,
	// interpolated). The classifier's start (`start`, a demodulation's carrier timing) wandered
	// +-1 ms from cycle to cycle on a clean channel and did not show the reference unit receiver's grid
	// moving -0.1 to -0.25 ms a cycle against ours (emulator, p2-we-call-we-send: the onsets of
	// its signals measured from the recordings drifted 1.5 ms against our packets while our
	// readings stayed within 3395.3 +-0.3 ms); our level-4 long packets then sat 1.5 ms late and
	// the firmware failed every one of them, memory ARQ and all, until its T (its window: late
	// 1.5 ms fails, early 1.2 ms passes). The onset follows the drift to ~0.1 ms. A reading it
	// cannot take (no clear rise) moves us by the drift estimate only. P2_LONG_ONSET=0: old.
	static constexpr double kLongGainOnset = 0.6, kLongRateGainOnset = 0.2;
	bool long_onset_mode_ = false;                    // fix 217: the readings are onsets (the follow's own gains)
	double long_onset_lag_ = 0;                       // the classifier's start minus the onset (last good pair; fallback)
	bool cs_onset(double coarse, double &onset);
	void long_follow(double off, bool good);
	double long_cand_ = 0;                            // fix 209: a clean place off the reference, not followed
	double long_rate_ = 0;                            // fix 209: the receiver's drift against our long cycle (samples a cycle)
	int long_cand_n_ = 0;
	int long_reads_ = 0;                              // long-cycle readings so far (test hook)
	double long_moved_ = 0;                           // our long-cycle start moved in all (samples; log)
	// Fix 231: as callee and sender (ISS) our packets follow the caller's control signals, in short
	// cycles too, and a jump in where they land is followed (fix 209's long-cycle follow refused it).
	// Why: the caller is the master. The reference unit as master keeps its own transmit grid; as
	// slave it moves its whole cycle, transmit and receive, slowly toward the master's bursts,
	// as receiver and as sender. So as callee it is our job to stay on the caller's grid.
	// We did as receiver (our answers sit on the packet we decode; fix 219 for the looks), but as
	// sender our short-cycle packets kept our own lattice, and in long cycles fix 209's rule (made for
	// us as master: the reference unit slave's grid never jumps) took a jump in the caller's signals as a
	// misread reference and re-took the reference without moving. When our sound card's capture slips
	// (the bench Pi, fix 219: 2-28 ms in 5-30 s) our packets then stay that far off the caller's
	// window for good: emulator, the firmware calls, we send, capture losing 18 ms in 7 s at 40 s:
	// P2 1,102 and P3 4,037 printed of 13,000+ (link down to level 1).
	// Now, as callee only (as master we must not follow a follower, fix 163 / fix 192):
	//  - short cycles: the same follow as fix 209's long cycles: a reference from the first clean
	//    reading, a clean reading near it steers our next packet proportional-integral (P2 from
	//    the signal's onset, fix 217's gains); a weak reading or none moves us by the drift estimate;
	//  - both: a jump is followed (fix 231c below has the rules that replaced fix 231's).
	//    The reference unit as slave never jumps. Our capture does, and a follow that slow
	//    would take hundreds of cycles to come back 18 ms.
	// Fix 231c (reference-unit bench, P3, no slip: two control signals read -5 and +19.8 ms off, each
	// followed at once because the audio layer had reported a 1 ms hold 22 s before; our packets sat
	// 15 ms off the reference unit's window: 1,951 of 12,000, timeout). On by default again
	// (CALLEE_FOLLOW=0 or CALLEE_FOLLOW_OFF=1: off), now with these rules:
	//  - a jump (a clear reading 1.5 ms or more off the reference) is followed only when the audio
	//    layer reported, within the last 20 s, a hold of about that size and sign that we have not
	//    followed yet (note_audio_slip(ms): within 3 ms or 30 %), AND the next clear reading agrees as
	//    well; it is then followed whole (at most 30 ms) and the report counts as used. A reading off
	//    the reference with no such report is never followed; three clear readings in a row at one new
	//    place (within 1 ms) re-take the reference there, without moving (the caller's signal moved,
	//    our clock did not: that run's readings after the jump stayed 15 ms on);
	//  - clear: P2 fix 209's test (hard 0.95, soft the same letter at 0.9, carriers not re-aligned); P3
	//    hard 0.95 with the soft letter the same (that run's +19.8 ms reading: hard Q, soft P);
	//  - short cycles, slow drift: the proportional-integral follow takes only clear readings within
	//    2 ms of the reference and moves at most 0.25 ms a cycle (130 ppm is 0.16 ms a cycle);
	//  - P2: the jump test reads the classifier's start (the onset, fix 217, failed on every bench
	//    reading, so P2 never followed its slip: 3,207 of 12,000).
	// An audio layer that does not report the size (note_audio_slip() without ms) gets no jump
	// followed at all.
	static constexpr double kCalleeJumpMaxS = 0.030; // the largest capture slips seen (fix 219): 24-28 ms
	static constexpr double kRtReportS = 20.0;       // how long a reported hold stays to be followed
	static constexpr double kCalleeStepS = 0.00025;  // short-cycle drift follow: at most per cycle
	static bool callee_follow_off()
	{
		static const bool v = std::getenv("CALLEE_FOLLOW_OFF") != nullptr || (std::getenv("CALLEE_FOLLOW") && std::atoi(std::getenv("CALLEE_FOLLOW")) == 0);
		return v;
	}
	double rt_pend_ = 0;                              // reported, not yet followed: where the signal should land now (samples)
	int64_t rt_rep_n_ = -1;
	bool p3_cs_clear(int cs) const { return look_hard_cs_ == cs && look_score_ >= 0.95 && (look_soft_cs_ < 0 || look_soft_cs_ == cs); }
	bool rt_agrees(double d) const
	{
		if (rt_rep_n_ < 0 || n_ - rt_rep_n_ > samples(kRtReportS) || std::fabs(rt_pend_) < double(samples(0.0015))) return false;
		const double tol = std::max(double(samples(0.003)), 0.3 * std::fabs(rt_pend_));
		return (d > 0) == (rt_pend_ > 0) && std::fabs(d - rt_pend_) <= tol;
	}
	struct CalleeJump { double ref = -1, alt = 0; int alt_n = 0; bool cand = false; };
	CalleeJump cj_short_, cj_long_;
	int cj_reads_ = 0;                                // test hook count
	double callee_test_shift();
	// The move for this reading (samples, 0: none). off: where the signal landed after our packet.
	double callee_jump(CalleeJump &J, double off, bool clear, const char *what);
	double sf_ref_ = -1, sf_rate_ = 0, sf_moved_ = 0;
	int64_t sf_next_ = -1;                            // where our next short-cycle packet goes (a run of them)
	// ref: our packet's start; have: a control signal was read; off: where it landed (samples after
	// ref); good: a clean reading; onset: off is the signal's onset (P2); jumped: callee_jump moved us.
	void callee_follow(int64_t ref, bool have, double off, bool good, bool onset, bool jumped = false);
	// Test (fault injection, sender side): P2_TEST_CSFAULT="U<n>+<k>=<L>,..." replaces the
	// k-th control signal after our n-th speed-up (k = 0: the answer to the first packet at
	// the new level) by letter L (P Q R S T, or - = nothing heard). Development only.
	int tf_ups_ = 0, tf_idx_ = -1;
	int test_csfault(int cs);
	// ISS: the receiver broke in; its break-in packet started at `start`.
	// arg bit 0: a retry (we asked for the break-in again), bit 1: a re-look
	struct BkRx { RxPacket p; bool again = false, retried = false; };
	bool bk_cc_ok_ = false; double bk_cc_[2] = {0, 0}; int64_t bk_cc_start_ = -1;   // fix 63: the tracked break-in's timing for its decode
	std::future<BkRx> bk_fut_, bk_late_;
	int64_t bk_fut_slot_ = -1, bk_late_slot_ = -1;
	int bk_rx_fails_ = 0;                             // fix 260: failed reads of the break-in being repeated to us
	static constexpr int kBkRxQ = 4;                  // fix 260: Q answers before T (the reference unit's count)
	BkRx bk_decode(int64_t start, bool retry, const Snap *sp, const TrkView *tv = nullptr);
	void on_bk_done(int64_t start, int arg = 0);

	void handle(const Event &e);
	// ---- P1 links (TODO O) ------------------------------------------------
	// The P1 station runs only while a P1 link (or a MYLevel 1 call) does. At a
	// hand-over it gets our raw receive audio since `from` replayed, so it holds
	// the packet it will read first, its clock in step with ours.
	// Fix 202: the replay starts where the P1 station first reads (`first_read`), less 0.1 s
	// for its discriminator to settle, instead of 1 s before the call. The replay runs on the
	// audio loop, inside the one sample that hands the link over: from 1 s before the call it
	// was 3.2 s of audio as callee (~16 ms on the x86 build machine; with the set-up packet's search and AFC,
	// ~35-41 ms in one tick), and on the bench Pi it took the hand-over past the 40 ms playback
	// queue: every P1 link the real reference unit called underran there, and the restart left our
	// replies ~9 ms late, outside the reference unit's window (the real-time audio loop, the same day).
	// Callee: 1.07 s (the caller's set-up packet); caller: 0.1 s (its next packet's answers lie
	// ahead). Nothing the P1 station reads lies earlier. P1_FULL_REPLAY=1: the old start.
	int64_t p1_replay_from(int64_t first_read) const
	{
		if (std::getenv("P1_FULL_REPLAY")) return p1_call_first_ - samples(1.0);
		return first_read - samples(0.1);
	}
	std::unique_ptr<P1Station> p1_;
	bool p1_link_ = false, p1_up_seen_ = false, p1_closing_ = false;
	int64_t p3_a1_at_ = 0;                            // fix 218: P3 callee, where the A1 header matched
	int64_t acc_first_ = -1;                          // fix 222: callee, the start of our first P2-/P3-accept
	// Fix 222: the callee's first PSK burst (P3 B1, P2 control signal; from queue_psk). The reference unit takes the
	// two-carrier order of a station's bursts as alternating every 1.25 s cycle from its set-up
	// accept, also over cycles it sent an extra accept in or nothing at all; the first PSK burst
	// one cycle after the accept leads low (psk_swap_ false). [bb-emu: the reference unit
	// callee whose caller's first A1 was cut answered it with P3-accept again and then B1 with
	// the carriers the other way round; our B1 with the usual order was never taken. The
	// caller's own bursts follow the same grid: its P2 packet after a P2-accept again leads high,
	// and P2 already answers in the order read from the packet (fix 168), so P2 was not hit.]
	// P3_B1_NO_PARITY=1: the old way (low after any number of cycles).
	void first_psk_parity(int64_t at);
	bool p2_setup_watch_ = false;                     // P2 callee: no good packet yet (our P2-accept may be lost: on_p1_again)
	int64_t p1_call_first_ = 0;
	void p1_new(int64_t from);
	// The P1 station has the link from here.
	void p1_take_over()
	{
		events_.clear();
		state_ = ST_P1; p1_link_ = true; p1_up_seen_ = true; p3_ = false;
		link_status(true, "");
		log("P1 link set up with " + remote_);
	}
	// The caller's call from its set-up packet's "<level><call>\r".
	std::string p1_caller_guess(const uint8_t *bytes) const;
	int16_t p1_tick(int16_t rx);
	void p1_report_end(const std::string &why)
	{
		log("disconnected: " + why);
		if (link_up_) link_status(false, " (" + why + ")");
		else link_events_.push_back({false, remote_});
	}
	void p1_end(const std::string &why)
	{
		if (p1_) rx_ += p1_->take_rx();
		if (!p1_closing_) p1_report_end(why);
		p1_.reset(); p1_closing_ = false;
		ptt_ = false;
		disconnect_quiet();
	}
	// Link up/down: a "*** ..." line in the received text (inline_status, the
	// CLI tools) and an event for host mode.
	void link_status(bool up, const std::string &note);
	int up_level_ = 1, up_reps_ = 2, up_counter_ = 1, up_rep_ = 0, up_baud_ = 100;
	int64_t up_slot_ = 0, up_idle_since_ = 0;
	bool up_first_ = false, up_p1_ = false, up_inv_ = false, up_long_ = false, up_p3_ = false, up_qrt_sent_ = false;
	static bool p3u_qrt_old() { static const bool v = std::getenv("P3U_QRT_OLD") != nullptr; return v; }   // fix 256 test: P3 unproto ends without its end packet
	uint8_t up_hdr_ = 0x55;
	double up_phase_ = 0;
	std::vector<uint8_t> up_words_, up_p1bits_;
	// One P1 unproto copy: 52.5 ms of the mark tone of its polarity, then the packet,
	// phase-continuous from the copy before (the transmission never stops).
	std::vector<float> up_p1_burst();
	void on_unproto(int64_t ref);
	// Fix 256: P3 unproto (modes 30-41), as the reference unit sends it (the emulated reference unit,
	// every mode recorded): the ARQ packets of the speed
	// level on P2 unproto's grid (a packet every 0.8875 s short, 3.3875 s long), each `reps` times,
	// the carrier map alternating with every transmission (the first even); the header by the
	// counter's parity; the first packet counter 3 (idle until text comes), then 0, 1, ...; the text
	// packed as ARQ packs it (the data type in status bits 2-4, 0x20 "more"). The pulses sit where
	// our ARQ packets put them (p3_pulse0), at our ARQ levels.
	void on_unproto_p3(int64_t ref);
	void unproto_end(const std::string &why)
	{
		log("unproto: " + why);
		want_qrt_ = false; txq_.clear();
		for (auto it = events_.begin(); it != events_.end();) it = it->type == EV_UNPROTO ? events_.erase(it) : it + 1;   // a new session starts its own chain
		state_ = answer_ ? ST_LISTEN : ST_IDLE;
	}
	void disconnect(const std::string &why);
	void disconnect_quiet();
	static std::string lower(std::string s) { for (auto &c : s) c = char(std::tolower(c)); return s; }

	// Sample i of a burst made in pieces (TxProg), shifted if its shift is set (the same numbers as
	// ssb_shift on the whole burst, kShiftChunk samples at a time); waits only if not made yet.
	static constexpr long kShiftChunk = 480;
	float prog_sample(TxBurst &b, size_t i);
	void prog_wait(TxBurst &b, size_t n);
	int16_t play();

	// ---- state ----------------------------------------------------------------
	P2Config cfg_;
	// the busy detector (busy_detector.hpp): standby only, frozen while we transmit
	BusyDetector busy_{double(cfg_.fs), double(cfg_.tone_lo), double(cfg_.tone_hi)};
	bool listen_cmd_ = false, was_standby_ = true;
	bool standby() const { return state_ == ST_IDLE || state_ == ST_LISTEN; }
	void busy_tick(float x)
	{
		const bool sb = standby();
		if (sb && !was_standby_) busy_.enter_standby();   // from a link back to standby
		was_standby_ = sb;
		if (sb && !ptt_) busy_.process(x, !listen_cmd_);
	}
	FskDiscriminator<> disc_;
	std::vector<float> raw_;
	std::vector<double> pre_;
	std::vector<uint8_t> ptt_hist_;                 // fix 243: our keying, per sample (only with rx_mute_delay)
	double gd_ = 0, T_ = 96;
	int nw_ = 154;
	int64_t n_ = 0;
	State state_ = ST_LISTEN;
	bool master_ = false, ptt_ = false, fsk_inv_ = false, debug_ = false, psk_swap_ = false;
	std::string remote_, txq_, rx_, log_;
	std::deque<LinkEvent> link_events_;
	bool link_up_ = false, answer_ = true;
	int64_t last_call_dump_ = 0;                    // (test: P1_CALL_DUMP)
	PanelStats pstats_;
	RxMeter meter_{double(cfg_.fs)};                // the front panel's S/N and audio level (measurement only)
	bool meter_link_ = false;
	int far_ = 0;                                   // the front panel: the last control signal's meaning (FAR_*)
	int64_t far_n_ = -1;
	void far_set(int f) { far_ = f; far_n_ = n_; }
	std::vector<uint8_t> bk_tx_;                   // our P3 break-in burst while it is unacknowledged
	text::RleExpander rle_;
	std::vector<SiMsg> si_rx_;                     // take_si()
	text::RleCompressor rle_tx_;                      // the send-side run-length coder
	size_t p3_greet_off_ = 0;                      // greeting characters already sent in B1
	// The callee's first turn as the reference unit (scs-modem TODO v): the connect text and the text queued
	// when B1 was made (greet_q_ bytes of txq_ then), B1 carrying its first 3; the greeting packet takes what
	// fits (greet_used_ of it, after B1's) and has no "over" if more is left (greet_more_): the caller then
	// acknowledges it and we go on sending, handing over by ourselves when the queue has run dry.
	std::string greet_txt_;
	size_t greet_q_ = 0, greet_used_ = 0;
	bool greet_more_ = false;
	static bool early_old() { static const bool o = std::getenv("P3_EARLY_OLD") != nullptr; return o; }   // the old way: the connect text alone
	// Typed characters the caller has now taken with B1 and the greeting: off txq_.
	void greet_taken()
	{
		const size_t gl = cfg_.p3_greeting.size(), done = p3_greet_off_ + greet_used_;
		const size_t n = std::min(greet_q_, done > gl ? done - gl : 0);
		if (n > 0) { txq_.erase(0, std::min(n, txq_.size())); sent_text_ = true; }
		greet_q_ = 0; greet_used_ = 0;
	}
	std::deque<Event> events_;
	std::vector<TxBurst> tx_;
	int64_t my_first_ = 0, my_start_ = 0, next_start_ = 0, grid_ = 0;
	int level_ = 2, rx_level_ = 2, counter_ = 0, last_rx_counter_ = -1, misses_ = 0, pending_used_ = 0;
	int64_t p1_late_ref_ = -1, p1_late_dec_ = -1, p3_b1_prev_ = -1; int p1_late_which_ = -1;   // fix 243: our last P1 packet whose answer we decided on
	int fsk_late_logs_ = 0;                                                    // fix 243: "our answer went out late" lines this link
	bool peer_signum_ = false;                                                // fix 269: late short bursts whole only to a Signum
	int psk_late_logs_ = 0;                                                    // fix 257: the same for short PSK bursts
	int late_cs_logs_ = 0;                                                     // fix 257: control signals read from their first steps
	bool long_ = false, want_qrt_ = false, pending_qrt_ = false, sent_text_ = false, must_break_in_ = false;
	// Owner decision a, P2/P3 (2026-11-15): break-in as receiver and "over" as sender as the reference unit.
	// It breaks in only on the sender's "over" (must_break_in_, fix 50), its break-in key (bk_req_: BKchr,
	// host %I/%O) or its QRT; with PDuplex 1 also on waiting text, but not within PDTimer s of the other
	// station's break-in. Its sender sets "over" only after the changeover key (cho_req_: CHOchr, host
	// %O) or, with PDuplex 1, on every packet while it has no text waiting (P1; P2/P3
	// [bb]: 0 characters without ^Y; the same rule assumed [inf]). P23_BK_AUTO_OLD=1: the old way (break
	// in whenever text waits, "over" once our text has gone); OVER_OLD=1: only the old "over".
	bool bk_req_ = false, cho_req_ = false;
	int64_t bk_hold_until_ = 0;                     // PDuplex: no break-in on waiting text before this (PDTimer)
	static bool bk_auto_old() { static const bool o = std::getenv("P23_BK_AUTO_OLD") != nullptr; return o; }
	static bool over_old() { static const bool o = bk_auto_old() || std::getenv("OVER_OLD") != nullptr; return o; }
	bool auto_break_in() const
	{
		if (txq_.empty()) return false;
		if (bk_auto_old()) return true;
		return cfg_.pduplex && n_ >= bk_hold_until_;
	}
	// "Over" on a new packet carrying `used` characters (0: our queue has run empty).
	bool over_due(int used) const
	{
		if (used != 0 || want_qrt_) return false;
		return over_old() ? sent_text_ : (cho_req_ || cfg_.pduplex);
	}
	// The other station broke in on us: we receive now; PDuplex's own break-ins wait PDTimer (the firmware
	// starts PDTimer at the other's P1 break-in; the same assumed for P2/P3 [inf]).
	void peer_broke_in(int64_t at) { cho_req_ = false; bk_hold_until_ = at + samples(double(cfg_.pdtimer_s)); }
	// Fix 61: an RMS ending the link after the CMS's FQ breaks in to send its
	// own QRT; breaking in again for ours at its next packet kept the link
	// ping-ponging until Pat's DD (KB5LZK 30 m, P2 and P3, 2026-10-16: every
	// session). After the peer takes the link back while our QRT is pending,
	// we wait kQrtHold of its packets for its QRT before breaking in again.
	static constexpr int kQrtHold = 8;
	int qrt_hold_ = 0;
	bool qrt_bk_now() { if (qrt_hold_ > 0) qrt_hold_--; return want_qrt_ && qrt_hold_ == 0; }
	// Fix 87: the hold also covers the peer's "over" (fix 50) when all we
	// would send is our QRT. KB5LZK (2026-10-18, P2 05:06 and P3 07:37) took
	// the link back after each of our QRT packets, then handed it to us with
	// "over" at once: we broke in again, 11-20 times, until Pat's DD (KC0TPS
	// 04:15: acknowledged only at the 31st try).
	bool qrt_hold_on_over() const { return want_qrt_ && qrt_hold_ > 0 && txq_.empty() && !std::getenv("P2_QRT_OVER_BK"); }
	// Fix 91: the peer that takes the link back after our QRT packet a third
	// time will not take our QRT: we end the link. On the air the RMS never
	// sent a QRT of its own after taking the link back (31 sessions with a
	// QRT, 2026-10-14 to 19); where it acknowledged ours it did so within 2
	// take-backs (7 sessions), and never after 3 (11 and 13, ended by Pat's
	// DD, which made Pat's driver panic). P2_QRT_TAKEBACKS=N: end at the Nth (0: never).
	int qrt_takes_ = 0;
	bool qrt_tx_since_take_ = false;
	static int qrt_takebacks_max() { const char *e = std::getenv("P2_QRT_TAKEBACKS"); return e && *e ? std::atoi(e) : 3; }
	// True when the link was ended here.
	bool qrt_peer_took_link();
	int last_rx_cs_ = -1, last_tx_cs_ = -1, ack_phase_ = 0, good_run_ = 0;
	bool last_pkt_full_ = false;                      // fix 217: our last new packet was full
	// Fix 217: no failed decode for kRxCleanS. The reference unit as receiver judges S and R packet
	// by packet and stops asking them in noise; our measure is the failed decode, so its
	// faster rules (R at once, P3 long packets, the run kept through R) apply only on a link that
	// has not failed lately. Without this gate the full set lost ground in noise: p2-snr3 2,339 ->
	// 2,243, p2-snr-3 1,186 -> 1,032, p2-moderate 2,122 -> 1,983, p3-snr9 6,441 -> 5,626.
	int64_t rx_last_fail_ = -(int64_t(1) << 40);
	static constexpr double kRxCleanS = 30.0;
	bool rx_clean() const { return n_ - rx_last_fail_ > samples(kRxCleanS); }
	bool p3_last_full_ = false;                       // fix 224: the previous new P3 packet held no fill
	int p3_run_ = 0;                                  // fix 229: good decodes since the last failure / break-in
	double p3_q_ = -1; int p3_q_lv_ = -1; bool p3_q_long_ = false;   // fix 229: the last decoded packet's quality reading
	bool rx_full_ = false, p3_full_ok_ = true;        // fix 217: the last packet received held no fill; the full trigger for R armed
	bool p2_quick_s_ = false;                         // fix 217: the first packet after the other end's break-in may get S at once
	int last_rx_metric_ = -1, last_rx_marq_ = 0;      // fix 217: the decode of the packet being answered
	int prev_rx_metric_ = -1;                         // fix 217: the one before
	// Fix 217: S only when the last two packets decoded with a Viterbi metric (corrected bits) at most
	// this (clean channel: 0 at every level and length). The reference unit as receiver asks S only
	// on packets it reads cleanly enough; without a gate our S
	// into a level the noise did not allow cost a T run each time (p2-snr-3: metrics 22-41 on L2 long,
	// S S S T T T and 23 s without a packet; p2-snr3: 15-34 on L3 long).
	static constexpr int kP2SMetric = 4, kP2SMetricLong = 12;
	static constexpr int kP2QuickSMetric = 4;         // fix 217: Viterbi metric of a "clean" short packet (clean channel: 0)
	bool rx_more_ = false, rx_long_ = false, qrt_rx_ = false, qrt_end_sent_ = false;
	int expect_ack_ = 0, rx_fails_ = 0, last_ack_cs_ = p2arq::CS_P;
	std::vector<uint8_t> pending_data_;
	uint8_t pending_status_ = 0;
	static constexpr double kP3SResid = 0.47;         // fix 224: S only with the level's decode residual at most this
	// Fix 229: S quality limits [short: fit residual, long: phase-step rms in 1/16 turn][level], at the noise
	// where the reference unit stops asking S at that level (pair runs, see reply_as_irs). 9 = no limit.
	static constexpr double kP3SQual[2][7] = {{9, 9, 0.47, 0.55, 0.46, 0.22, 0}, {9, 9, 9, 1.12, 0.77, 0.43, 0}};
	static constexpr int kP3TDown = 5;                // P3: failures before T, as the reference unit
	// Header toggle: flips with each new packet and restarts at every speed-down
	// (the firmware's sender restarts it there); counter parity otherwise.
	int hdr_flip_ = 0;
	int64_t bk_ans_at_ = -1; int bk_ans_cap_ = 0;
	int64_t a2_ans_at_ = -1; int a2_ans_cap_ = 0;   // fix 229: our A2 answer, chosen when its data has decoded
	mutable double bk_bias_ = 0;                     // the last matched break-in's offset (steps, absolute)   // our answer to a break-in, chosen after its decode (fix 42)
	bool p1_slow_ = false;                          // P1 link at 100 Bd (call answered with CS4)
	uint8_t p1_hdr_ = 0xAA;                         // our set-up packet's header
	bool p1_sd_ = false;                            // our set-up packet was sped down by CS4 (fix 82)
	int hdr_counter() const { return counter_ ^ hdr_flip_; }
	// header words per header pulse, P3 levels (tx_words: the header toggle)
	static constexpr int kHdrWords[7] = {0, 0, 6, 14, 16, 18, 18};
	int p3_t_run_ = 0;                                // T's since our last speed-down (0: no T episode)
	bool rx_t_state_ = false;
	int rx_t_run_ = 0;                                // P2 receiver: T's sent since the sender's expected last speed-down
	static constexpr int kP3SpeedUp = 2;              // the firmware's MAXUp (4) - 2 in P3
	static constexpr int kP2SpeedUp = 4;              // the firmware's MAXUp (4) in P2
	bool p2_up_pending_ = false;
	// P2 receiver: failures before our T (experiment, fix 171 measures it; the firmware: 5)
	static int p2_rx_tdown() { static const int v = std::getenv("P2_RX_TDOWN") ? std::atoi(std::getenv("P2_RX_TDOWN")) : 2; return v; }
	int rep_same_ = 0;                                // fix 170: repeats of a held packet answered with one letter
	static constexpr int kRepeatProbe = 4;
	int p2_up_fails_ = 0, p2_up_bad_ = 0, p2_up_from_ = 0, p2_pre_s_ = p2arq::CS_P, p2_req_cs_ = p2arq::CS_S;
	int p2_backoff_[2] = {1, 1};                     // S, R: request after kP2SpeedUp x this many good packets
	int p3_backoff_ = 1;                             // P3 S: request after kP3SpeedUp x this many good packets (fix 65)
	int rx_good_ = 0, rx_up_fails_ = 0;
	bool rx_up_pending_ = false;
	int rx_up_from_ = 1;                            // the receive level to fall back to when a speed-up is not taken (fix 81)
	int rx_up_cs_ = p2arq::CS_S;                     // the level request pending (S, or our T/S answer to a break-in: fix 46)
	int rx_pre_s_ = p2arq::CS_P;                         // receiver: we sent T and have had no good packet since
	bool r_pending_ = false;                          // our last acknowledgement was R (length switch pending)
	int r_run_ = 0;                                   // repeated R's since
	double long_cs_off_ = -1;                         // where the receiver's control signal lands in our long cycle
	double long_jump_ = 0;                            // fix 77: a jump of it not yet followed (samples)
	int p3_up_fails_ = -1;                            // 0: our speed-up on S awaits its first answer, -1: none
	int p3_ans_cs_ = -1;                              // our break-in was answered with this T/S; awaiting the receiver's Q (fix 48)
	bool p3_a2_rx_t_ = false;                         // fix 258: as receiver, the A2 answered (no plain acknowledgement since while last_ack_cs_ is T)
	int p3_pre_s_ = p2arq::CS_P;                      // the receiver's control signal before its S
	bool p3_ = false, p3_first_tx_ = true;
	int p3_level_ = 5, p3_rx_level_ = 5;
	int last_rx_type_ = 0;
	int led_ir_ = 0, led_te_ = 0, led_good_role_ = -1, led_tx_type_ = 0;   // front panel (led_*() above)
	// A packet check's result for the panel statistics: 0 good, 1 error, 2 repeat.
	void pk_count(int r)
	{
		(r == 0 ? pstats_.pk_ok : r == 1 ? pstats_.pk_err : pstats_.pk_rep)++;
		pstats_.q_hist = (pstats_.q_hist << 1) | (r == 0 ? 1u : 0u);
		pstats_.q_n++;
	}
	int led_marq_ = 0;                                    // 1 = decoded by memory ARQ, 2 = a copy added (MARQ-OK / MARQ-IN)
	bool led_rx_data_ = false, led_rx_idle_ = false;
	int64_t led_trk_n_ = -1;
	bool p3_long_ = false;                   // P3 sender: long-cycle packets (the receiver asked with R)
	std::future<p3::RxPacket> p3_fut_;
	// decode margin and back-off (the design notes 2, 3): when each worker finished
	std::shared_ptr<std::atomic<int64_t>> p3_done_, p3l_done_, p2_done_;
	std::shared_ptr<std::atomic<bool>> p2_short_ok_;   // fix 203: the short look's decode found a short packet
	DecodeStats dstats_;
	std::string link_note_;
	bool throttled_ = false;
	int p3_cap_ = 6;                                  // the P3 receive level cap (6: none)
	int64_t cap_hold_ = 0;                            // the cap stays until this sample
	bool late_sl1_logged_ = false;                    // fix 242: logged once per link
	bool tx_step_logged_ = false;                     // test hook TX_TEST_STEP: logged once
	int caps_ = 0;                                    // caps set this link: each one holds twice as long as the last
	int64_t last_cap_at_ = -1;                        // fix 226: the sample of the last cap this link (-1: none)
	static constexpr double kCapGraceS = 8.0;         // fix 226: lateness this soon after a cap does not cap again
	static constexpr double kCapBurstS = 10.0;        // fix 226: caps this close count once for the hold doubling
	static constexpr double kThinMs = 8.0;            // a margin below this: do not climb
	static constexpr double kCapHoldS = 120.0;        // a cap is lifted after this long without trouble (doubled per cap this link, at most 8x)
	static int64_t wall_ns() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
	void dstats_push(float v) { dstats_.recent[size_t(dstats_.rn)] = v; dstats_.rn = (dstats_.rn + 1) % int(dstats_.recent.size()); }
	// A decode done `done` (wall ns), its deadline the sample `deadline`: the margin.
	void note_margin(int level, int64_t deadline, int64_t done, bool p3);
	void note_late(int level, bool p3);
	// Fix 264 (on air 2026-11-18, bench Pi 4): ONE late decode capped the receive level for 120 s, and the
	// reference unit, which has no such rule, went on sending the higher levels: nothing decoded, the link
	// starved. The late decodes there were scheduling stalls, not a shortage of compute [measured: underruns
	// of "a period's work took 0.2 ms", the result 86-168 ms after launch while an SL1 decode's own work was
	// ~15 ms, a capture that had just lost 117 ms]. A computer that is too slow is late again on the next
	// packet (one every 1.25 s cycle: two late decodes are ~2.5 s apart), so the back-off now wants EVIDENCE:
	// two late or thin decodes within kEvidenceS (10 s, ~8 cycles), none of them within kHiccupSlipS of an
	// audio stall or underrun (note_audio_slip: the capture lost time or the playback restarted, a cause
	// outside the decode; a stall long enough to make two decodes late is told this way). The first is
	// logged as a hiccup and changes nothing. What the cap then does is as before (fix 226).
	// One history for the short decodes (kind 0, the level cap) and one for the long ones (kind 1, the long
	// ceiling). P3_BACKOFF_OLD=1: every late or thin decode counts at once, the hold 120 s, as before.
	static constexpr double kEvidenceS = 10.0;        // fix 264: two late or thin decodes this close are a shortage
	static constexpr double kHiccupSlipS = 2.0;       // fix 264: a decode this soon after an audio stall is a hiccup, not counted
	static constexpr double kCapHoldFirstS = 60.0;    // fix 264: the first cap of a link holds this long at least (then doubled per cap)
	int64_t ev_at_[2] = {-1, -1};                     // fix 264: the last late or thin decode (kinds 0, 1) that was not a stall's
	static bool backoff_old() { static const bool v = std::getenv("P3_BACKOFF_OLD") != nullptr; return v; }
	bool backoff_evidence(int kind, const std::string &what);
	void cap_to(int lv, const std::string &why);
	// a cap's hold: 120 s, then 240, 480, 960 s for the next caps of the same link (as the
	// speed-up back-off): a computer that is slow all along is not tried every 2 minutes
	// (fix 264: from 60 s, half the link timeout, so a cap that was not needed cannot outlast the link: the
	// caps now come only after evidence, and a computer that is slow all along is back at 120, 240, 480 s)
	double cap_hold_s() const { return (backoff_old() ? kCapHoldS : kCapHoldFirstS) * double(1 << std::min(3, std::max(0, caps_ - 1))); }
	void relax()
	{
		if (p3_cap_ < 6 && n_ >= cap_hold_ && !throttled_) { p3_cap_ = 6; log("P3: computer back-off lifted (keeping up for " + std::to_string(int(cap_hold_s())) + " s)"); }
	}
	p3::RxPacket p3_res_;
	int64_t p3_res_slot_ = -1;
	bool p3_res_late_ = false;
	std::future<RxPacket> sl1_fut_;
	int64_t sl1_slot_ = -1;
	int64_t p3_fut_slot_ = -1;
	// last: destroyed first, finishing its queued decodes while the rest
	// of the station (which they read) still exists
	DecodePool pool_{SIGNUM_DECODE_THREADS};
	// the early P3 timing searches (launch_p3_sync): their own workers, so that searches running
	// below the decodes' priority never hold the workers the decodes, our transmit audio and the
	// looks need (one core: the searches are preempted and take longer). P3_SCHED_OLD=1: pool_.
	DecodePool sync_pool_{std::max(0, SIGNUM_DECODE_THREADS - 1)};
	LoadCounters load_;
	int tx_late_ = 0;   // transmit bursts whose audio the audio loop had to wait for
};

}  // namespace fmodem

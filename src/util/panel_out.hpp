// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// Front-panel state of the station as JSON lines (rendered by tui/signum-tui and the GUI).
// LED meanings and timing follow the V4.1r reference unit (seen on its front panel and in
// the emulator): speed level on DQPSK/DBPSK + MaxSpeed/HiSpeed, compression on Compress 2/1 (PMC green,
// Huffman red) for the packet sent or received, Idle/Request and
// Traffic/Error per packet or acknowledgement check, Send green while we are
// the sender and CHO red after a change of direction, Tracking green when a
// frequency correction was made this cycle, Connected while linked. Standby
// is all dark; calling is Send green + HiSpeed red. MARQ: red while a
// corrupt copy has been added to the memory-ARQ sum, green when a packet
// decoded only from the sum (fix 52). The display shows plain text.
// The OFDM build also writes the Mercury modem's state (update_ofdm) while it
// is the modem in use; each line says which ("modem").
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>

struct PanelOut {
	FILE *f = nullptr;
	std::string last, last_line;                     // last: the state compared; last_line: as sent (with "t")
	std::function<void(const std::string &)> sink;   // set: lines go there (a background writer) instead of f
	std::function<void(const std::string &)> ws_sink;   // set: every line also goes there (the WebSocket interface), with or without f
	bool active() const { return f || ws_sink; }
	// The radio's frequency (Hz) from the rig control the PTT uses (src/radio/ptt.hpp, rig:): set by the program;
	// < 0: no rig control, 0: not known (yet, or no answer for 10 s). The panel's "freq_khz".
	double rig_hz = -1;
	bool open(const char *path) { f = std::fopen(path, "w"); return f != nullptr; }
	// settings: the station's MODEM settings (OFDM build), shown by the TUI.
	template <class S> void update(const S &st, double t, const std::string &settings = "")
	{
		if (!active()) return;
		const int lv = st.speed_level(), p3 = st.link_up() && st.link_plevel() == 3;
		// P3: SL1 red/-, SL2 green/-, SL3 -/red, SL4 -/green, SL5 red/green, SL6 green/green.
		// P2 levels 1-4 use the first four rows (the manual: "identical to P2").
		static const char *const psk[7] = {"off", "red", "green", "off", "off", "red", "green"};
		static const char *const spd[7] = {"off", "off", "off", "red", "green", "green", "green"};
		const int plv = st.link_up() ? st.link_plevel() : 0;   // 1, 2, 3 (a MYLevel 1 link is a P1 link)
		const int i = plv == 1 || lv < 0 || lv > 6 ? 0 : lv;   // P1 is FSK: the PSK/speed rows off (inferred)
		const int ty = st.panel_data_type();
		const char *comp = !st.link_up() ? "off" : ty >= 4 ? "green" : (ty == 1 || ty == 2) ? "red" : "off";
		// As the firmware's display: "---STBY---", "P3 N0AAA" while linked ("P1 ..." on a P1 link: inferred).
		std::string text = st.link_up() ? "P" + std::to_string(plv) + " " + st.remote() : st.setting_up() ? "CALL " + st.remote() : "---STBY---";
		std::string j = "{\"src\":\"cpp\",\"modem\":\"signum\",\"text\":\"" + text + "\",\"leds\":{";
		auto add = [&](const char *k, const char *v, bool last_one = false) { j += std::string("\"") + k + "\":\"" + v + "\"" + (last_one ? "" : ","); };
		static const char *const gr[3] = {"off", "green", "red"};
		const bool calling = st.setting_up() && !st.link_up();
		add("Idle/Request", gr[st.led_idle_req()]);
		add("Traffic/Error", gr[st.led_traffic_err()]);
		const bool snd = st.led_send() || st.p1_sending();   // a P1 link: we hold the sending direction (inferred)
		add("Send/CHO", snd && st.led_cho() ? "yellow" : snd ? "green" : st.led_cho() ? "red" : "off");
		add("Compress 2/1", comp);
		add("Tracking/Phasing", st.led_tracking() ? "green" : "off");
		add("MARQ-OK/MARQ-IN", gr[st.led_marq()]);
		add("DQPSK/DBPSK", psk[i]);
		add("MaxSpeed/HiSpeed", calling ? "red" : spd[i]);
		add("Connected/Mail", st.link_up() ? "yellow" : "off", true);
		// Mode lamps (not on the real panel; user request 2026-10-16): the
		// level in use and the speed level within it. P1 = the call and
		// set-up phase (FSK, level 1 = 100 Bd, 2 = 200 Bd); P2 levels 1-4; P3
		// SL1-6. Standby: none.
		int mode = 0, mlv = 0, mmax = 0;
		if (st.link_up() && plv == 1) { mode = 1; mlv = st.p1_baud() == 100 ? 1 : 2; mmax = 2; }
		else if (st.link_up()) { mode = p3 ? 3 : 2; mlv = lv; mmax = p3 ? 6 : 4; }
		else if (st.p1_phase()) { mode = 1; mlv = st.p1_baud() == 100 ? 1 : 2; mmax = 2; }
		else if (st.setting_up()) { mode = 3; mlv = 1; mmax = 6; }       // P3 set-up after the P1 phase
		// The busy lamp's state (the channel busy detector, any standby, Listen too): "busy" / "free",
		// "-" while a link is up or being set up. Not drawn here: the front ends place it.
		{ const int b = st.busy_lamp(); j += std::string("},\"busy\":\"") + (b > 0 ? "busy" : b == 0 ? "free" : "-") + "\""; }
		// per direction (the TX and RX rows): tx/rx = speed level (P1: 1 = 100 Bd, 2 = 200 Bd), 0 = none; tx_long/rx_long:
		// long packets that way. `level` stays the TX level, as before, for older front ends.
		int mtx = mlv, mrx = mlv;
		if (st.link_up() && plv != 1) mrx = st.rx_speed_level();
		const bool ltx = st.link_up() && st.tx_long_packets(), lrx = st.link_up() && st.rx_long_packets();
		j += ",\"mode\":{\"plevel\":" + std::to_string(mode) + ",\"level\":" + std::to_string(mlv) + ",\"levels\":" + std::to_string(mmax) +
		     ",\"tx\":" + std::to_string(mtx) + ",\"rx\":" + std::to_string(mrx) + ",\"tx_long\":" + (ltx ? "true" : "false") +
		     ",\"rx_long\":" + (lrx ? "true" : "false") + "}";
		// What the receiver hears (owner, 2026-11-22; rx_meter.hpp), standby too: the receive audio level, and the
		// radio's frequency (rig control): null when there is none or it is not known
		{
			const auto &mt = st.rx_meter();
			static const char *const lvn[5] = {"none", "low", "ok", "high", "clip"};
			char a[160];
			std::snprintf(a, sizeof a, ",\"audio\":{\"state\":\"%s\",\"peak_dbfs\":%.1f,\"rms_dbfs\":%.1f}", lvn[mt.level() + 1], mt.peak_dbfs(), mt.rms_dbfs());
			j += a;
			if (rig_hz > 0) { std::snprintf(a, sizeof a, ",\"freq_khz\":%.2f", rig_hz / 1000.0); j += a; }
			else j += ",\"freq_khz\":null";
		}
		const auto &ps = st.panel_stats();
		auto letters = [](const std::array<int, 5> &c) { std::string r; for (int k = 0; k < 5; k++) r += (k ? "," : "") + std::to_string(c[size_t(k)]); return r; };
		auto letter = [](int cs) { return cs >= 0 && cs < 5 ? std::string(1, "PQRST"[cs]) : std::string("-"); };
		// decode margin and back-off (the design notes 2, 3): ms, null: none measured yet this link
		const auto &ds = st.decode_stats();
		auto ms = [](double v) { if (std::isnan(v)) return std::string("null"); char num[24]; std::snprintf(num, sizeof num, "%.1f", v); return std::string(num); };
		std::string dm = ",\"margin_ms\":" + ms(ds.last_ms) + ",\"margin_min\":" + ms(ds.min_ms) + ",\"margin_med\":" + ms(ds.median_recent()) +
		                       ",\"late\":" + std::to_string(ds.late) + ",\"late_sl1\":" + std::to_string(ds.late_sl1) + ",\"late_bk\":" + std::to_string(ds.late_bk) + ",\"p3_cap\":" + std::to_string(st.p3_level_cap());
		// the received S/N in dB (in 3 kHz; null: not measurable), how the far end hears us (the meaning of its last
		// control signal while we send: ok, repeat, faster, slower, length), characters a minute each way (the last 60 s)
		{
			double db = 0;
			char a[48];
			if (st.rx_snr(db)) { std::snprintf(a, sizeof a, ",\"snr_db\":%.1f", db); dm += a; } else dm += ",\"snr_db\":null";
			// where the reading comes from (the window's own gaps, or the held noise reference while we send) and the
			// reference's age in seconds and origin (the idle standby, or the gaps of this link): for the tooltip
			const auto &mt = st.rx_meter();
			double age = 0;
			dm += st.rx_snr(db) ? (mt.snr_from_ref() ? ",\"snr_src\":\"ref\"" : ",\"snr_src\":\"gaps\"") : ",\"snr_src\":null";
			if (mt.noise_ref_age(age)) {
				std::snprintf(a, sizeof a, ",\"snr_ref_s\":%.0f,\"snr_ref_kind\":\"%s\"", age, mt.ref_from_gaps() ? "gaps" : "idle");
				dm += a;
			} else dm += ",\"snr_ref_s\":null,\"snr_ref_kind\":null";
			static const char *const fn[5] = {"ok", "repeat", "faster", "slower", "length"};
			const int fe = st.far_end();
			dm += ",\"far\":[";
			for (int k = 0, c = 0; k < 5; k++) if (fe >> k & 1) { dm += std::string(c++ ? "," : "") + "\"" + fn[k] + "\""; }
			dm += "]";
			const long ack = long(ps.tx_text) - long(st.tx_unsent()) - long(st.tx_unacked());
			int rin = 0, rout = 0;
			rates(st.link_up(), t, long(ps.rx_bytes), std::max(0L, ack), rin, rout);
			dm += ",\"rx_cpm\":" + std::to_string(rin) + ",\"tx_cpm\":" + std::to_string(rout);
		}
		char b[768];
		std::snprintf(b, sizeof b, ",\"detail\":{\"mycall\":\"%s\",\"remote\":\"%s\",\"role\":\"%s\",\"state\":\"%s\",\"unsent\":%zu,\"unacked\":%zu,"
		              "\"settings\":\"%s\",\"afc_hz\":%.1f,\"cs_last_rx\":\"%s\",\"cs_last_tx\":\"%s\",\"cs_rx\":[%s],\"cs_tx\":[%s],"
		              "\"rx_bytes\":%zu,\"tx_text\":%zu,\"link_s\":%.0f,\"plevel\":%d,\"speed\":%d,\"ptt\":%s,"
		              "\"qrt\":%s,\"pk_ok\":%d,\"pk_err\":%d,\"pk_rep\":%d,\"quality\":%d}}",
		              st.mycall().c_str(), st.remote().c_str(), st.link_up() || st.setting_up() ? (st.caller() ? "caller" : "callee") : "-",
		              st.link_up() ? "connected" : st.setting_up() ? "calling" : "standby", st.tx_unsent(), st.tx_unacked(), settings.c_str(),
		              st.panel_afc_hz(), letter(st.last_cs_rx()).c_str(), letter(st.last_cs_tx()).c_str(), letters(ps.cs_rx).c_str(), letters(ps.cs_tx).c_str(),
		              ps.rx_bytes, ps.tx_text, st.link_seconds(), st.link_up() ? st.link_plevel() : 0, st.link_up() ? lv : 0,
		              st.ptt() ? "true" : "false",   // the PTT lamp: the modem transmitting (Send/CHO is the protocol state)
		              // qrt: a D (orderly disconnect) asked for and the link (or call) not yet ended: the
		              // disconnect button's lamp. pk_*: this link's packet checks; quality: % good of the last 16.
		              st.qrt_pending() && (st.link_up() || st.setting_up()) ? "true" : "false",
		              ps.pk_ok, ps.pk_err, ps.pk_rep, st.link_quality());
		j += b;
		j.insert(j.size() - 2, dm);                     // into "detail" (before its "}}")
		emit(j, t);
	}
	// Another modem behind the same host interface (another modem: the
	// Mercury OFDM modem, ARDOP), in the same stream: the modem writes its own
	// line ("modem":"ofdm" / "ardop", panel_json) and tui/signum-tui
	// switches its layout on it.
	template <class M> void update_aux(const M &m, double t, const std::string &settings = "")
	{
		if (!active()) return;
		emit(m.panel_json(t, settings), t);
	}

	// A modem's line made elsewhere (a packet modem: "packet").
	void update_json(const std::string &j, double t) { if (active()) emit(j, t); }

private:
	// Characters a minute each way over the last 60 s of the link (from its start while shorter): one sample a second.
	struct RateAt { double t; long in, out; };
	std::array<RateAt, 61> rate_{};
	int rate_n_ = 0, rate_head_ = 0;
	bool rate_up_ = false;
	void rates(bool up, double t, long in, long out, int &rin, int &rout)
	{
		rin = rout = 0;
		if (up != rate_up_) { rate_up_ = up; rate_n_ = 0; rate_head_ = 0; }
		if (!up) return;
		if (rate_n_ == 0 || t - rate_[size_t((rate_head_ + 60) % 61)].t >= 1.0) {
			rate_[size_t(rate_head_)] = {t, in, out};
			rate_head_ = (rate_head_ + 1) % 61;
			if (rate_n_ < 61) rate_n_++;
		}
		const RateAt &o = rate_[size_t((rate_head_ + 61 - rate_n_) % 61)];
		const double dt = t - o.t;
		if (dt < 2.0) return;
		rin = int(std::lround(double(in - o.in) / dt * 60.0));
		rout = int(std::lround(double(std::max(0L, out - o.out)) / dt * 60.0));
	}
	void emit(const std::string &j, double t)
	{
		if (j == last) return;
		last = j;
		char tb[32];
		std::snprintf(tb, sizeof tb, ",\"t\":%.3f}\n", t);
		const std::string line = j.substr(0, j.size() - 1) + tb;
		last_line = line;
		if (ws_sink) ws_sink(line);
		if (!f) return;
		if (sink) { sink(line); return; }
		std::fputs(line.c_str(), f);
		std::fflush(f);
	}
};

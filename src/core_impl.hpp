// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// core_impl.hpp: signum::Core (src/tnc_core.hpp) over the modem: the station,
// one HostIface per stream client (the pty is client 0), the front panel, the
// listen-mode monitor. service() runs once per audio period on the loop's
// thread: it hands received text and link events to every client, writes
// the panel, and applies what is pending.
#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "tnc_core.hpp"
#include "host_iface.hpp"
#include "station.hpp"
#include "panel_out.hpp"
#include "monitor_thread.hpp"
#include "cpu_health.hpp"
#include "json_mini.hpp"
#include "ic7300_power.hpp"

#include <atomic>
#include <mutex>
#include <thread>
#if defined(SIGNUM_CRYPTO) && !defined(SIGNUM_NO_CRYPTO)
#include "crypto/crypto_control.hpp"
#endif

namespace signum {

class CoreImpl : public Core {
public:
	CoreImpl(const fmodem::P2Config &cfg, const HostParams &par, const std::string &version);
	~CoreImpl() override
	{
		mon_.stop();
		if (rx_trace_) std::fclose(rx_trace_);
		if (icp_th_.joinable()) icp_th_.join();
		if (icp_running_) { radio::ic7300::power::busy() = false; hold_ptt(false); }
	}

	Station &station() { return stn_; }
	TuneTone &tune() { return sh_.tune; }
	PeerId &peer() { return sh_.peer; }
	// The ALC warning (src/alc_warn.hpp): the audio loop tells it when our transmitter keys and unkeys.
	AlcWarn &alc() { return sh_.alc; }
	void alc_key(bool keyed)
	{
		const fmodem::P2Station &s = stn_.st();
		sh_.alc.key(keyed, s.link_up() ? s.link_plevel() : 0, s.speed_level(), double(stn_.params().tx_gain_db), last_t_);
	}

	// ---- the audio loop's side: one sample in, one out, and the key ---------------------
	// The modem's, except while the tuning tone before our call sounds (src/tune_tone.hpp):
	// then the tone goes out, and the modem keeps its clock hearing silence (not our own tone
	// through a loopback, nor a call it could not answer while we transmit).
	int16_t tick(int16_t rx)
	{
		if (!sh_.tune.active()) return st().tick(rx);
		(void)st().tick(0);
		return sh_.tune.tick();
	}
	bool key() const { return stn_.st().ptt() || sh_.tune.keyed(); }   // the transmitter keyed (the modem or the tuning tone)
	fmodem::P2Station &st() { return stn_.st(); }

	// ---- CPU health (src/cpu_health.hpp): is this computer keeping up --------------------
	CpuHealth &health() { return health_; }
	// The audio loop's own signs of lateness (the modem's counters are polled in service()).
	// n: samples so far at 9600 Hz. Real-time (ALSA) runs only.
	void cpu_event(CpuHealth::Kind k, uint64_t n, double ms = 0) { health_.event(k, double(n) / 9600.0, ms); }
	// Set by the program: the load average over 1 minute and the core count (false: unknown);
	// read every 5 s of audio time, never in the modem.
	std::function<bool(double &, int &)> load_probe;

	// ---- the loop's side ---------------------------------------------------------
	// Once per audio period (n: samples so far at 9600 Hz; rx/tx: this period's audio).
	void service(uint64_t n, const int16_t *rx, const int16_t *tx, size_t len);
	// The front panel's frequency readout (the PTT's rig control, src/radio/ptt.hpp): the program reads it only in
	// standby (no call, no link, no tuning tone) and passes it here (Hz; -1: no rig control, 0: not known).
	bool rig_standby() { return !st().link_up() && !st().setting_up() && !sh_.tune.keyed(); }
	void rig_freq(double hz) { panel_.rig_hz = hz; }
	// Log lines (the modem's, the hosts'): to stdout (the program) and the front ends.
	std::function<void(const std::string &)> log_out;

	// ---- Core --------------------------------------------------------------------
	std::string version() const override { return "Signum " + version_; }
	std::string mycall() const override { return stn_.st().mycall(); }
	const std::string &panel_json() const override { return panel_json_; }
	void on_panel(std::function<void(const std::string &)> f) override { on_panel_ = std::move(f); }
	int attach(const std::string &name) override
	{
		const int id = next_id_++;
		hosts_[id].reset(new HostIface(sh_));
		log_line("[client] " + std::to_string(id) + " attached (" + name + ")\n");
		return id;
	}
	void detach(int c) override { if (c != 0 && hosts_.erase(c)) log_line("[client] " + std::to_string(c) + " detached\n"); }
	void input(int c, const uint8_t *p, size_t n) override { auto it = hosts_.find(c); if (it != hosts_.end()) it->second->input(p, n); }
	std::string take_output(int c) override { auto it = hosts_.find(c); return it == hosts_.end() ? std::string() : it->second->take_out(); }
	bool host_mode(int c) const override { auto it = hosts_.find(c); return it != hosts_.end() && it->second->host_mode(); }
	std::string command(const std::string &line) override { return hosts_.at(0)->command(line); }
	void connect(const std::string &call) override
	{
		std::string c = call;
		for (auto &x : c) x = char(std::toupper(uint8_t(x)));
		sh_.last_call = c; sh_.calling = (c.size() > 1 && (c[0] == '!' || c[0] == ';')) ? c.substr(1) : c;
		if (sh_.tune.active() || st().link_up() || st().setting_up()) return;
		sh_.place_call(c);
	}
	// (held encrypted data goes first.) No link: nothing, as the console's D (before, the request stayed pending
	// in the modem and ended the NEXT link at once).
	void disconnect() override
	{
		if (sh_.tune.cancel()) return;
		if (!st().link_up() && !st().setting_up() && !st().unproto()) return;
		if (!(sh_.take_qrt && sh_.take_qrt())) st().qrt();
	}
	void abort() override { if (!sh_.tune.cancel()) st().abort_link(); }
	void send(const std::string &text) override { if (sh_.link_send) sh_.link_send(text); else st().send_text(text); }
	bool changeover(bool break_in_only) override
	{
		if (!st().link_up()) return false;
		return st().changeover_key(break_in_only ? fmodem::P1Station::KEY_BK : fmodem::P1Station::KEY_BK | fmodem::P1Station::KEY_CHO);
	}
	std::string link_json() const override;
	void on_rx(std::function<void(const std::string &)> f) override { on_rx_ = std::move(f); }
	void on_link(std::function<void(bool, const std::string &)> f) override { on_link_ = std::move(f); }
	void on_log(std::function<void(const std::string &)> f) override { on_log_ = std::move(f); }
	void on_monitor(std::function<void(const std::string &, const std::string &)> f) override { on_monitor_ = std::move(f); }
	void on_audio(std::function<void(const int16_t *, const int16_t *, size_t)> f) override { on_audio_ = std::move(f); }
	std::string settings_json() const override;
	std::string set(const std::string &key, const std::string &v) override;
	// The startup settings shown by settings_json (the program fills them).
	std::string alsa, ptt, pty;
	double max_key_s = 90;                                 // the key-down watchdog (s)
	std::function<void(double)> on_max_key;                // the program: change the PTT watchdog
	// The settings file (the program provides them: src/start_config.hpp).
	std::function<std::string()> startup_json_fn;
	std::function<std::string(const std::string &, const std::string &)> startup_set_fn;
#if defined(SIGNUM_CRYPTO) && !defined(SIGNUM_NO_CRYPTO)
	crypto::CryptoControl &crypto() { return crypto_; }
	bool crypto_built() const override { return true; }
	std::string crypto_status() const override { return crypto_.status_json(); }
	std::string crypto_request(const std::string &j) override
	{
		jsonm::Value v;
		if (!jsonm::parse(j, v) || v.kind != jsonm::Value::OBJ) return "\"ok\":false,\"text\":\"not a JSON object\"";
		const std::string r = crypto_.request(v);
		crypto_dirty_ = true;
		return r;
	}
	void on_crypto(std::function<void()> f) override { on_crypto_ = std::move(f); }
#endif
	std::string startup_json() const override { return startup_json_fn ? startup_json_fn() : Core::startup_json(); }
	std::string startup_set(const std::string &k, const std::string &v) override { return startup_set_fn ? startup_set_fn(k, v) : Core::startup_set(k, v); }
	bool quiet() const override { return stn_.quiet() && !sh_.tune.active(); }
	std::function<void(bool)> on_hold_ptt;                 // the program: hold the PTT unkeyed
	void hold_ptt(bool h) override { if (on_hold_ptt) on_hold_ptt(h); }

private:
	// ---- ICPower W: the IC-7300's RF POWER for a wanted P2/P3 peak (src/radio/ic7300_power.hpp, the model only) ----
	// Runs on a thread of its own (CI-V takes seconds) with the PTT held unkeyed; ICPower alone shows the result.
	std::thread icp_th_;
	std::atomic<bool> icp_done_{false};
	bool icp_running_ = false;
	std::mutex icp_mu_;
	std::vector<std::string> icp_lines_;
	std::string icp_state_ = "NOT ASKED";
	double icp_gain_ = 0;                                // the TX gain the job chose (applied on the loop's thread)
	bool icp_gain_set_ = false;
	std::string icpower_cmd(const std::string &arg);
	void icpower_service();                               // on the loop's thread: the job's end
#if defined(SIGNUM_CRYPTO) && !defined(SIGNUM_NO_CRYPTO)
	void crypto_init(const std::string &version);
	crypto::CryptoControl crypto_;
	bool crypto_dirty_ = false;
	std::function<void()> on_crypto_;
#endif
	FILE *rx_trace_ = nullptr;
	FILE *rx_audio_ = nullptr;
	void log_line(const std::string &s, double t = -1);
	// The panel line (PanelOut's JSON + "t"), plus the tuning indicator: 15 LEDs, the centre
	// one (7) for no offset, one LED per 5 Hz [inferred: the reference unit lights one LED per step of
	// its tuning error; the step size is assumed], none while no signal is tracked.
	// "bar" carries it too: bit 14 - i = LED i of 15 (front ends read tune_led / freq_led first).
	void panel_line(const std::string &line);

	// Turns the modem's cumulative load counters (P2Station::load_counters) into health events.
	void poll_load(double t);

	// The peer marker (src/peer_id.hpp): this link's state from the moment the modem has it up; the marker
	// once, while we hold the sending direction (host_text: just before the host's text for the link).
	// With encryption on, its own record is the marker.
	void peer_begin()
	{
		if (sh_.peer.begun() || !st().link_up()) return;
		bool record = false;
#if defined(SIGNUM_CRYPTO) && !defined(SIGNUM_NO_CRYPTO)
		record = crypto_.cfg.active();
#endif
		sh_.peer.begin(st().link_plevel(), record);
	}
	void peer_marker(bool host_text)
	{
		peer_begin();
		if (!sh_.peer.due() || !st().link_up() || !(host_text || st().sending())) return;
		st().send_si(PeerId::marker_body(version_));
		sh_.peer.sent();
		log_line("[peer] marker sent (" + PeerId::marker_body(version_) + ")\n", last_t_);
	}

	// The tuning tone (src/tune_tone.hpp): its log lines, the panel when it keys or unkeys, and the
	// call once the tone and the gap after it are over.
	void tune_service(double t);

	static constexpr double kTxLateMs = 15.0;
	bool tune_keyed_ = false;
	Station stn_;
	CpuHealth health_;
	fmodem::P2Station::LoadCounters prev_;
	double last_t_ = 0, load_t_ = -100;
	HostShared sh_;
	std::string version_;
	std::map<int, std::unique_ptr<HostIface>> hosts_;
	int next_id_ = 1;
	PanelOut panel_;
	std::string panel_json_;
	MonitorThread mon_;
	bool listen_was_ = false;
	std::function<void(const std::string &)> on_panel_, on_rx_, on_log_;
	std::function<void(bool, const std::string &)> on_link_;
	std::string last_reason_, last_call_;           // the last link's end and call (link_json)
	std::function<void(const std::string &, const std::string &)> on_monitor_;
	std::function<void(const int16_t *, const int16_t *, size_t)> on_audio_;
};

}  // namespace signum

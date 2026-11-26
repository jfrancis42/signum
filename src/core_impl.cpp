// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// CoreImpl member functions (out of core_impl.hpp, so that they build in parallel).
#include "core_impl.hpp"

namespace signum {

CoreImpl::CoreImpl(const fmodem::P2Config &cfg, const HostParams &par, const std::string &version)
	: stn_(cfg), version_(version)
{
	stn_.params() = par;
	if (par.maxerr != 70 || par.txdelay != 4 || par.pska != 140 || par.tx_gain_db != 0) stn_.params_changed();
	sh_.stn = &stn_;
	sh_.version_text = "Signum " + version + " - P1/P2/P3 HF data modem, classic host-mode interface\r\n";
	// the tones the station starts with, as TOnes reports them
	const float lo = cfg.tone_lo, hi = cfg.tone_hi;
	sh_.tones = lo == 1400.0f && hi == 1600.0f ? 4 : lo == 1200.0f && hi == 1400.0f ? 0 : lo == 2100.0f && hi == 2300.0f ? 1 : 2;
	sh_.mark = int(hi); sh_.space = int(lo);
	panel_.ws_sink = [this](const std::string &l) { panel_line(l); };
	sh_.cpu_status = [this]() { return health_.console(last_t_); };
	sh_.icpower = [this](const std::string &a) { return icpower_cmd(a); };
	hosts_[0].reset(new HostIface(sh_));       // the pty (client 0)
	// Host data for the link: the peer marker first when it is due (src/peer_id.hpp), then the encryption
	// layer when built in (it holds or encrypts), else straight to the modem.
	sh_.link_send = [this](const std::string &x) {
		peer_marker(true);
#if defined(SIGNUM_CRYPTO) && !defined(SIGNUM_NO_CRYPTO)
		crypto_.link.host_send(x);
#else
		st().send_text(x);
#endif
	};
	if (const char *f = std::getenv("SIGNUM_RX_TRACE")) rx_trace_ = std::fopen(f, "ab");   // tests: the link text as received, before any decryption
	if (const char *f = std::getenv("SIGNUM_RX_AUDIO")) rx_audio_ = std::fopen(f, "wb");   // diagnostics: the received audio (s16le, 9600 Hz)
#if defined(SIGNUM_CRYPTO) && !defined(SIGNUM_NO_CRYPTO)
	crypto_init(version);
#endif
}

void CoreImpl::service(uint64_t n, const int16_t *rx, const int16_t *tx, size_t len)
{
	const double t = double(n) / 9600.0;
	last_t_ = t;
	poll_load(t);
	if (on_audio_) on_audio_(rx, tx, len);
	if (rx_audio_) std::fwrite(rx, sizeof *rx, len, rx_audio_);
	// listen mode: the monitor hears what we hear while idle
	tune_service(t);
	icpower_service();
	const bool listen_now = stn_.params().listen != 0 && stn_.quiet() && !sh_.tune.active();
	if (listen_now && !mon_.running()) mon_.start(stn_.config());
	if (!listen_now && listen_was_) mon_.reset();
	listen_was_ = listen_now;
	st().set_listen(stn_.params().listen != 0);    // the busy detector gives no busy in Listen mode
	if (listen_now && mon_.running()) mon_.push(rx, len);
	if (mon_.running() && n % 960 < len)
		for (const auto &l : mon_.take()) {
			const std::string txt = l.who.empty() ? "*** LISTEN: " + l.text : l.who + "> " + l.text;
			for (auto &h : hosts_) h.second->notice(txt);
			if (on_monitor_) on_monitor_(l.who, l.text);
			log_line("[listen] " + txt + "\n");
		}
	if (st().link_up()) { sh_.had_link = true; sh_.last_level = st().link_plevel(); }
	std::string r = st().take_rx();
	auto si = st().take_si();
	auto ev = st().take_link_events();
	{ std::string c; if (sh_.tune.take_aborted(c)) ev.push_back({false, c}); }   // the tuning tone aborted: the call's end
	if (rx_trace_ && !r.empty()) { std::fwrite(r.data(), 1, r.size(), rx_trace_); std::fflush(rx_trace_); }
	// Who the other station is (src/peer_id.hpp): from the text and SIs as received, before any decryption.
	{
		bool ch = false;
		for (const auto &e : ev) if (!e.up && sh_.peer.begun()) { sh_.peer.end(); ch = true; }
		if (!sh_.peer.begun() && st().link_up()) { peer_begin(); ch = true; }
		ch = sh_.peer.text(r.size()) || ch;
		for (const auto &x : si)
			if (sh_.peer.si(x.body)) { ch = true; log_line("[peer] the other station is a Signum" + (sh_.peer.version().empty() ? std::string() : " " + sh_.peer.version()) + "\n", t); }
		if (ch && sh_.peer.kind() == PeerId::OTHER)
			log_line("[peer] no marker in the other station's first " + std::to_string(PeerId::kOtherAfter) + " characters: not a Signum with its marker on\n", t);
		if (ch) panel_.last.clear();                 // the lamps: the panel goes out again
		st().set_peer_signum(sh_.peer.begun() && sh_.peer.kind() == PeerId::SIGNUM);   // fix 269
	}
#if defined(SIGNUM_CRYPTO) && !defined(SIGNUM_NO_CRYPTO)
	// The encryption layer: a new link first, then the received text (decrypted, or held back), then an end.
	for (const auto &e : ev) if (e.up) crypto_.link.link_up(e.call, st().master(), st().link_plevel(), t);
	{
		std::vector<std::pair<size_t, std::string>> s2;
		for (auto &x : si) s2.emplace_back(x.pos, std::move(x.body));
		r = crypto_.link.rx(r, s2, t);
	}
	for (const auto &e : ev) if (!e.up) crypto_.link.link_down(t);
	crypto_.link.tick(t);
	if (crypto_dirty_ && on_crypto_) { crypto_dirty_ = false; on_crypto_(); }
#else
	(void)si;
#endif
	peer_marker(false);                          // the marker, once we hold the sending direction
	for (auto &h : hosts_) h.second->deliver(r, ev);
	// The order every client sees: a link coming up, then the text received in this pass (it belongs to
	// that link: e.g. the caller's first text in its set-up packet, taken at link set-up), then a link
	// ending. Text before the link-up event was dropped by host programs as received with no link.
	auto link_event = [&](const fmodem::P2Station::LinkEvent &e) {
		if (e.up) last_reason_.clear();
		else {                                      // why it ended (the modem's note, without its brackets)
			std::string w = st().link_note();
			while (!w.empty() && (w[0] == ' ' || w[0] == '(')) w.erase(0, 1);
			while (!w.empty() && w.back() == ')') w.pop_back();
			last_reason_ = w.empty() ? "ended" : w;
		}
		last_call_ = e.call;
		if (on_link_) on_link_(e.up, e.call);
	};
	for (const auto &e : ev) if (e.up) link_event(e);
	if (!r.empty() && on_rx_) on_rx_(r);
	for (const auto &e : ev) if (!e.up) link_event(e);
	std::string lg = st().take_log();
	for (auto &h : hosts_) lg += h.second->take_log();
	if (!lg.empty()) log_line(lg, t);
	stn_.tick_idle();                         // parameters that need a new station, once idle
	if (health_.update(t)) { panel_.last.clear(); }      // a new CPU state or reason: the panel goes out again
	{ const std::string l = health_.take_log(); if (!l.empty()) log_line(l, t); }
	sh_.alc.service(t, st().link_up(), st().link_plevel());
	{ const std::string l = sh_.alc.take_log(); if (!l.empty()) log_line(l, t); }
	if (sh_.alc.take_dirty()) panel_.last.clear();      // the ALC card: the panel goes out again
	if (n % 480 < len) panel_.update(st(), t);   // the panel, every 50 ms (written on change)
}

std::string CoreImpl::link_json() const
{
	const fmodem::P2Station &s = stn_.st();
	const bool up = s.link_up(), setup = s.setting_up() || sh_.tune.active();
	const size_t held = sh_.held ? sh_.held() : 0;
	const auto &ps = s.panel_stats();
	std::string j = "{\"state\":\"" + std::string(up ? "connected" : setup ? "calling" : "standby") + "\",\"call\":" +
	                jsonm::quote(up || setup ? s.remote() : last_call_) + ",\"role\":\"" + (up || setup ? (s.caller() ? "caller" : "callee") : "") +
	                "\",\"plevel\":" + std::to_string(up ? s.link_plevel() : 0) + ",\"level\":" + std::to_string(s.speed_level()) +
	                ",\"unsent\":" + std::to_string(s.tx_unsent() + held) + ",\"unacked\":" + std::to_string(s.tx_unacked()) +
	                ",\"held\":" + std::to_string(held) + ",\"rx_bytes\":" + std::to_string(ps.rx_bytes) + ",\"tx_bytes\":" + std::to_string(ps.tx_text);
	char b[48];
	std::snprintf(b, sizeof b, ",\"link_s\":%.1f", s.link_seconds());
	return j + b + ",\"reason\":" + jsonm::quote(up || setup ? std::string() : last_reason_) + "}";
}

std::string CoreImpl::settings_json() const
{
	const auto &s = stn_.st();
	const HostParams &p = stn_.params();
	char b[1536];
	std::snprintf(b, sizeof b,
	              "{\"mycall\":\"%s\",\"mylevel\":%d,\"tone_lo\":%.0f,\"tone_hi\":%.0f,\"mode\":%d,\"listen\":%d,\"maxerr\":%d,\"txdelay\":%d,"
	              "\"pska\":%d,\"fska\":%d,\"maxup\":%d,\"maxdown\":%d,\"maxtry\":%d,\"maxsum\":%d,\"hostch\":%d,\"cpu_adaptive\":%d,\"p3_long_ceiling\":%d,"
	              "\"tune_s\":%d,\"tune_hz\":%d,\"tune_db\":%d,\"tune_method\":\"%s\",\"tx_gain\":%.1f,\"max_key\":%.0f,\"pduplex\":%d,\"pdtimer\":%d,"
	              "\"alc_warn\":%d,\"peer_marker\":%d,\"cmsg\":%d,\"status\":%d,\"escchr\":%d,\"qrtchr\":%d,\"bkchr\":%d,\"chochr\":%d,\"urep\":%d,"
	              "\"alsa\":\"%s\",\"ptt\":\"%s\",\"pty\":\"%s\"}",
	              s.mycall().c_str(), s.p1_only() ? 1 : s.allow_p3() ? 3 : 2, double(s.tone_lo()), double(s.tone_hi()), s.tx_mode(), p.listen,
	              p.maxerr, p.txdelay, p.pska, p.fska, p.maxup, p.maxdown, p.maxtry, p.maxsum, p.hostch, s.p3_long() ? 1 : 0, s.p3_long_ceiling(),
	              sh_.tune.secs, sh_.tune.hz, sh_.tune.db, TuneTone::method_name(sh_.tune.method), double(p.tx_gain_db), max_key_s, s.pduplex() ? 1 : 0, s.pdtimer(),
	              sh_.alc.enabled ? 1 : 0, sh_.peer.marker ? 1 : 0, hosts_.at(0)->cmsg(), p.status, p.escchr, p.qrtchr, p.bkchr, p.chochr, sh_.up_reps,
	              alsa.c_str(), ptt.c_str(), pty.c_str());
	return b;
}

std::string CoreImpl::set(const std::string &key, const std::string &v)
{
	static const std::map<std::string, std::string> verb = {
	    {"mycall", "MYcall"}, {"mylevel", "MYLevel"}, {"mode", "MOde"}, {"listen", "Listen"}, {"maxerr", "MAXErr"}, {"txdelay", "TXDelay"},
	    {"pska", "PSKAmpl"}, {"fska", "FSKAmpl"}, {"maxup", "MAXUp"}, {"maxdown", "MAXDown"}, {"maxtry", "MAXTry"}, {"maxsum", "MAXSum"},
	    {"hostch", "PTChn"}, {"tone_lo", "SPAce"}, {"tone_hi", "MARk"}, {"cpu_adaptive", "CPUadapt"}, {"pduplex", "PDuplex"}, {"pdtimer", "PDTimer"},
	    {"cmsg", "CMsg"}, {"peer_marker", "PEERmark"}, {"alc_warn", "ALCwarn"}, {"status", "STatus"}, {"escchr", "ESCchr"}, {"qrtchr", "QRTChr"}, {"bkchr", "BKchr"}, {"chochr", "CHOchr"}};
	if (key == "tx_gain" || key == "max_key" || key == "urep") {   // no terminal command of their own (urep: U *n, which is silent during a link)
		char *e = nullptr;
		const double d = std::strtod(v.c_str(), &e);
		const bool num_ok = e != v.c_str() && !*e && std::isfinite(d);
		char b[96];
		if (key == "tx_gain") {
			if (!num_ok || d < -40 || d > 20) return "*** BAD ARGUMENT (TX GAIN -40 TO 20 DB)\r\n";
			stn_.params().tx_gain_db = float(d);
			stn_.params_changed();                    // now when idle, else after the link
			std::snprintf(b, sizeof b, "*** TX GAIN (ON TOP OF PSKAMPL): %.1f DB\r\n", d);
			return b;
		}
		if (key == "max_key") {
			if (!num_ok || d < 1 || d > 600) return "*** BAD ARGUMENT (KEY-DOWN WATCHDOG 1 TO 600 S)\r\n";
			max_key_s = d;
			if (on_max_key) on_max_key(d);
			std::snprintf(b, sizeof b, "*** KEY-DOWN WATCHDOG: %.0f S\r\n", d);
			return b;
		}
		if (!num_ok || d < 1 || d > 5 || d != std::floor(d)) return "*** BAD ARGUMENT\r\n";
		sh_.up_reps = int(d);
		return "*** UNPROTO-REPETITION-TIME: " + std::to_string(sh_.up_reps) + "\r\n";
	}
	if (key == "tune_s" || key == "tune_hz" || key == "tune_db" || key == "tune_method") {   // TUnetone S HZ DB METHOD, the others as they are
		const TuneTone &t = sh_.tune;
		const std::string s = key == "tune_s" ? v : std::to_string(t.secs), h = key == "tune_hz" ? v : std::to_string(t.hz), d = key == "tune_db" ? v : std::to_string(t.db),
		                  m = key == "tune_method" ? v : std::string(TuneTone::method_name(t.method));
		return command("TUNETONE " + s + " " + h + " " + d + " " + m);
	}
	auto it = verb.find(key);
	if (it == verb.end()) return "*** ERROR: " + key + " cannot be set here";
	std::string r = command(it->second + " " + v);
	if ((key == "tone_lo" || key == "tone_hi") && r.find("ERROR") == std::string::npos && r.find("BAD") == std::string::npos) r += command("TOnes 2");
	return r;
}

std::string CoreImpl::icpower_cmd(const std::string &arg)
{
	namespace pw = radio::ic7300::power;
	const std::string head = "*** IC-7300 POWER: ";
	if (arg.empty()) {
		std::lock_guard<std::mutex> g(icp_mu_);
		std::string s = head + icp_state_;
		for (const auto &l : icp_lines_) s += "\r\n    " + l;
		return s;
	}
	char *e = nullptr;
	const double w = std::strtod(arg.c_str(), &e);
	std::string rest = e ? e : "";
	while (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
	for (auto &c : rest) c = char(std::toupper(static_cast<unsigned char>(c)));
	if (e == arg.c_str() || !(w > 0)) return "*** INVALID VALUE: ICPOWER W (THE WANTED P2/P3 PEAK POWER IN WATTS)";
	if (!rest.empty() && std::string("MEASURE").compare(0, rest.size(), rest) == 0)
		return head + "MEASURE NEEDS THE SOUND CARD THIS SIGNUM HOLDS: STOP SIGNUM AND RUN signum --ic7300-power W --measure";
	if (!rest.empty()) return "*** INVALID VALUE: ICPOWER W";
	if (icp_running_) return head + "BUSY (ICPOWER ALONE SHOWS THE RESULT)";
	if (!quiet()) return head + "NOT DURING A LINK OR WHILE TRANSMITTING";
	bool expect = false;
	if (!pw::busy().compare_exchange_strong(expect, true)) return head + "THE RADIO IS BUSY (THE GUI'S IC-7300 DIALOG)";
	pw::Drive d;
	d.tx_gain_db = stn_.params().tx_gain_db;
	d.pska = stn_.params().pska;
	char b[160];
	std::snprintf(b, sizeof b, "WORKING ON %.1f W PEAKS (TX GAIN %+.1f DB, PSKAMPL %d)", w, d.tx_gain_db, d.pska);
	{ std::lock_guard<std::mutex> g(icp_mu_); icp_state_ = b; icp_lines_.clear(); }
	if (icp_th_.joinable()) icp_th_.join();
	hold_ptt(true);                                  // opening the radio's port clears RTS/DTR (our PTT's lines)
	icp_running_ = true; icp_done_ = false;
	icp_th_ = std::thread([this, w, d]() {
		namespace ic = radio::ic7300;
		std::vector<std::string> out;
		std::string state;
		const char *root = std::getenv("SIGNUM_TEST_IC7300_ROOT");   // tests: a fake /dev + /sys tree
		const auto all = ic::find(root ? root : "");
		if (all.empty()) state = "NO IC-7300 FOUND";
		else if (all.size() > 1) state = "MORE THAN ONE IC-7300: USE THE GUI OR signum --ic7300-power W --ic7300-serial N";
		else {
			util::SafePort port;
			const pw::Result r = pw::set_power(port, all[0], w, d, nullptr, [&out](const std::string &, const std::string &, int) {});
			char t[120];
			if (r.ok) std::snprintf(t, sizeof t, "SET %d/255 FOR %.1f W PEAKS (WAS %d/255)", r.after, w, r.before);
			else std::snprintf(t, sizeof t, "NOT SET (NOTHING CHANGED%s)", r.backup.empty() ? "" : "; A BACKUP WAS SAVED");
			state = t;
			out = r.lines;
			if (r.lines.empty() && !r.error.empty()) out.push_back(r.error);
			if (!r.backup.empty()) out.push_back("Backup (restores RF POWER and the USB MOD Level too): " + r.backup);
			if (r.ok) { icp_gain_ = r.gain_after; icp_gain_set_ = true; }
		}
		{ std::lock_guard<std::mutex> g(icp_mu_); icp_state_ = state; icp_lines_ = out; }
		icp_done_ = true;
	});
	return head + b + "; ICPOWER ALONE SHOWS THE RESULT";
}

void CoreImpl::icpower_service()
{
	if (!icp_running_ || !icp_done_) return;
	if (icp_th_.joinable()) icp_th_.join();
	icp_running_ = false;
	hold_ptt(false);
	radio::ic7300::power::busy() = false;
	std::lock_guard<std::mutex> g(icp_mu_);
	if (icp_gain_set_) {                             // the drive chosen: live now, and in the settings file
		icp_gain_set_ = false;
		char v[24];
		std::snprintf(v, sizeof v, "%g", icp_gain_);
		const double was = stn_.params().tx_gain_db;
		if (std::fabs(was - icp_gain_) > 1e-6) set("tx_gain", v);
		const std::string e = startup_set("tx-gain", v);
		char t[200];
		std::snprintf(t, sizeof t, "TX gain %+.1f dB -> %+.1f dB now; settings file: %s", was, icp_gain_, e.empty() ? ("tx-gain = " + std::string(v)).c_str() : ("NOT saved (" + e + ")").c_str());
		icp_lines_.push_back(t);
	}
	log_line("[ic7300] ICPower: " + icp_state_ + "\n", last_t_);
}

#if defined(SIGNUM_CRYPTO) && !defined(SIGNUM_NO_CRYPTO)
void CoreImpl::crypto_init(const std::string &version)
{
	crypto_.link.version = version;
	crypto_.link.modem_send = [this](const std::string &x) { st().send_text(x); };
	crypto_.link.modem_si = [this](const std::string &x) { st().send_si(x); };
	crypto_.link.modem_qrt = [this]() { st().qrt(); };
	crypto_.link.modem_abort = [this]() { st().abort_link(); };
	crypto_.link.modem_unsent = [this]() { return st().tx_unsent(); };
	crypto_.link.modem_sending = [this]() { return st().sending(); };
	crypto_.link.modem_break_in = [this]() { return st().changeover_key(fmodem::P1Station::KEY_BK); };
	crypto_.link.modem_changeover = [this]() { return st().changeover_key(fmodem::P1Station::KEY_CHO); };
	crypto_.link.log = [this](const std::string &l) { log_line("[crypto] " + l + "\n", last_t_); };
	crypto_.link.changed = [this]() { crypto_dirty_ = true; };
	crypto_.mycall = [this]() { return st().mycall(); };
	crypto_.link_is_up = [this]() { return st().link_up(); };
	sh_.take_qrt = [this]() { return crypto_.link.take_qrt(); };
	sh_.held = [this]() { return crypto_.link.held(); };
	sh_.unproto_refused = [this]() {
		return crypto_.cfg.on ? std::string("*** ERROR: UNPROTO REFUSED: ENCRYPTION IS ON (a broadcast cannot be encrypted; CRYPTO OFF first)\r\n")
		     : crypto_.cfg.verify ? std::string("*** ERROR: UNPROTO REFUSED: THE IDENTITY CHECK IS ON (a broadcast cannot be verified; CRYPTO OFF first)\r\n") : std::string();
	};
	sh_.crypto_cmd = [this](const std::string &a) { const std::string r = crypto_.command(a); crypto_dirty_ = true; return r; };
}
#endif

void CoreImpl::log_line(const std::string &s, double t)
{
	std::string o;
	if (t >= 0) {
		char tb[24]; std::snprintf(tb, sizeof tb, "%8.3f ", t);
		size_t a = 0;
		while (a < s.size()) { size_t b = s.find('\n', a); if (b == std::string::npos) b = s.size() - 1; o += tb + s.substr(a, b - a + 1); a = b + 1; }
	} else o = s;
	if (log_out) log_out(o);
	if (on_log_) on_log_(o);
}

void CoreImpl::panel_line(const std::string &line)
{
	std::string j = line.substr(0, line.find_last_not_of('\n') + 1);
	int lit = -1;
	const auto &s = st();
	if (s.link_up()) lit = 7 + std::max(-7, std::min(7, int(std::lround(s.panel_afc_hz() / 5.0))));
	const unsigned bar = lit >= 0 ? 1u << (14 - lit) : 0u;
	if (!j.empty() && j.back() == '}') {
		j.pop_back();
		j += ",\"bar\":" + std::to_string(bar) + ",\"tune_led\":" + std::to_string(lit) +
		     ",\"freq_led\":\"" + std::string(lit >= 0 && std::fabs(s.panel_afc_hz()) > 10.0 ? "red" : "off") + "\",\"cpu\":" + health_.json(last_t_) +
		     ",\"peer\":" + sh_.peer.json() + ",\"alc\":" + sh_.alc.json() + "}";
	}
	if (sh_.tune.keyed()) {                      // the tuning tone: the transmitter is keyed (the TX lamp)
		const size_t k = j.find("\"ptt\":false");
		if (k != std::string::npos) j.replace(k, 11, "\"ptt\":true");
	}
	panel_json_ = j;
	if (on_panel_) on_panel_(panel_json_);
}

void CoreImpl::poll_load(double t)
{
	const auto &c = st().load_counters();
	if (c.late < prev_.late || c.thin < prev_.thin || c.caps < prev_.caps || c.tx_late < prev_.tx_late ||
	    c.sl1_rescue < prev_.sl1_rescue || c.fallbacks < prev_.fallbacks || c.late_bk < prev_.late_bk)
		prev_ = {};                                     // a new station (MAXErr/TXDelay/PSKAmpl rebuilt it): count from zero
	auto feed = [&](long now, long &was, CpuHealth::Kind k, double ms) {
		for (long i = 0, m = std::min(now - was, 8L); i < m; i++) health_.event(k, t, ms);
		was = now;
	};
	feed(c.late, prev_.late, CpuHealth::LATE, 0);
	feed(c.late_bk, prev_.late_bk, CpuHealth::LATE, 0);
	feed(c.thin, prev_.thin, CpuHealth::THIN, std::isnan(c.last_thin_ms) ? 0 : c.last_thin_ms);
	feed(c.caps, prev_.caps, CpuHealth::BACKOFF, 0);
	feed(c.fallbacks, prev_.fallbacks, CpuHealth::BACKOFF, 0);
	feed(c.sl1_rescue, prev_.sl1_rescue, CpuHealth::SL1, 0);
	// A transmit burst's audio is normally 4-7 ms short of its start (the audio loop waits for it,
	// well inside the playback cushion): only a wait that eats the cushion is a sign of lateness.
	if (c.last_tx_wait_ms >= kTxLateMs) feed(c.tx_late, prev_.tx_late, CpuHealth::TXLATE, c.last_tx_wait_ms);
	else prev_.tx_late = c.tx_late;
	// The CPU fallbacks (CPUadapt, scs-modem TODO 5): the modem drops CPU-bound extras while the state
	// is not OK, and says which it used.
	st().set_cpu_health(int(health_.state()));
	{
		const int m = st().cpu_fallbacks();
		std::vector<std::string> f;
		if (m & fmodem::P2Station::FB_P3_LEVELS) f.push_back("P3 packets tried at the expected level only");
		if (health_.set_fallbacks(f)) panel_.last.clear();
	}
	if (load_probe && t - load_t_ >= 5.0) {
		load_t_ = t;
		double l = 0; int cores = 0;
		if (load_probe(l, cores)) health_.set_load(l, cores);
	}
}

void CoreImpl::tune_service(double t)
{
	sh_.tune.service();
	std::string l;
	if (sh_.tune.take_note(l)) log_line("[tune] " + l + "\n", t);
	if (sh_.tune.keyed() != tune_keyed_) { tune_keyed_ = sh_.tune.keyed(); panel_.last.clear(); }
	std::string c;
	if (sh_.tune.take_call(c)) {
		if (stn_.quiet()) st().connect(c);
		else log_line("[tune] the call to " + c + " not placed: the station is busy\n", t);
	}
}

}  // namespace signum

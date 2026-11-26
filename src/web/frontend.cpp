// frontend.cpp: signum's front ends on one port: the web GUI over HTTP
// (www/index.html), the JSON WebSocket API the GUI and the TUI speak (the
// protocol is in src/web/README.md), and the command shell on a WebSocket: /shell, a raw byte
// stream attached to the command interpreter exactly as a pty client is
// (terminal mode and JHOST1/JHOST4 host mode).
//
// Threading (tnc_core.hpp): everything here that touches the Core runs in
// frontend_pump(), on the core's loop thread. The server (ws_server.hpp)
// has its own thread; the two meet only in its try_lock queues, so the
// pump never blocks.
//
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <thread>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "../tnc_core.hpp"
#include "ws_server.hpp"
#include "../util/json_mini.hpp"
#include "../util/waterfall.hpp"
#include "../radio/ic7300.hpp"
#include "../radio/ic7300_power.hpp"
#include "../radio/ptt.hpp"

#include <mutex>

namespace signum {
namespace {

// ---- options -------------------------------------------------------------------
struct Options {
	bool on = true;
	std::string addr = "127.0.0.1";
	int port = 8770;
	std::string web_auth, ws_auth;     // "user:pass", "" = open
	std::string gui;                   // directory served over HTTP ("" = found at start)
} opt;

bool read_cred_file(const std::string &path, std::string &out, std::string &err)
{
	FILE *f = std::fopen(path.c_str(), "r");
	if (!f) { err = "cannot read " + path; return false; }
	char b[512];
	out.clear();
	while (std::fgets(b, sizeof b, f)) {
		std::string l = b;
		while (!l.empty() && (l.back() == '\n' || l.back() == '\r' || l.back() == ' ')) l.pop_back();
		if (l.empty() || l[0] == '#') continue;
		out = l;
		break;
	}
	std::fclose(f);
	struct stat st{};
	if (::stat(path.c_str(), &st) == 0 && (st.st_mode & 077)) std::fprintf(stderr, "[ws] warning: %s can be read by others\n", path.c_str());
	if (out.find(':') == std::string::npos) { err = path + ": expected USER:PASS"; return false; }
	return true;
}

bool dir_has_index(const std::string &d)
{
	struct stat st{};
	return !d.empty() && ::stat((d + "/index.html").c_str(), &st) == 0;
}

// The GUI directory: --gui, else <binary>/../www, <binary>/www, ./www, then the compiled-in data directory.
std::string find_gui()
{
	if (!opt.gui.empty()) return opt.gui;
	char exe[PATH_MAX] = {0};
	const ssize_t k = ::readlink("/proc/self/exe", exe, sizeof exe - 1);
	if (k > 0) {
		std::string d(exe, size_t(k));
		d = d.substr(0, d.rfind('/'));
		for (const std::string &c : {d + "/../www", d + "/www", d + "/../share/signum/www"})
			if (dir_has_index(c)) return c;
	}
	if (dir_has_index("www")) return "www";
#ifdef SIGNUM_DATADIR
	if (dir_has_index(SIGNUM_DATADIR "/www")) return SIGNUM_DATADIR "/www";   // compiled in by ./configure
#endif
	return "";
}

// JSON text from the GUI arrives as UTF-8; the reference unit stream is bytes. Code points
// U+0000-U+00FF become one byte each (the Latin-1 rule of src/web/README.md);
// anything else (not representable) becomes '?'.
std::string utf8_to_latin1(const std::string &s)
{
	std::string o;
	for (size_t i = 0; i < s.size();) {
		const unsigned char c = (unsigned char)s[i];
		uint32_t cp; int n;
		if (c < 0x80) { cp = c; n = 1; }
		else if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) { cp = (c & 0x1Fu) << 6 | ((unsigned char)s[i + 1] & 0x3Fu); n = 2; }
		else if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) { cp = 0x800; n = 3; }
		else if ((c & 0xF8) == 0xF0 && i + 3 < s.size()) { cp = 0x10000; n = 4; }
		else { cp = c; n = 1; }   // not UTF-8 at all: the byte as it is
		o += cp < 0x100 ? char(cp) : '?';
		i += size_t(n);
	}
	return o;
}

double now_s()
{
	static const auto t0 = std::chrono::steady_clock::now();
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

// ---- the front end ----------------------------------------------------------------
struct Client {
	bool authed = false;
	bool shell = false;     // a /shell client: raw bytes, attached at once
	int core_id = 0;        // the core's stream client (shell, or the web terminal after term_open)
	bool wf = false;        // asked for the waterfall
};

struct LinkRec {          // this run's links (the GUI's link list; not kept across runs)
	double start = 0, end = 0;
	long long unix_start = 0, unix_end = 0;
	std::string call, role;
	int plevel = 0, level_max = 0, level_last = 0;
	long rx_bytes = 0, tx_bytes = 0;
};

class Frontend {
public:
	Core *core = nullptr;
	wss::Server ws;
	std::map<int, Client> cl;
	std::string rx_hist;                       // the last 4 KB received (sent to a new client)
	std::deque<std::string> log_ring;          // the last log lines (JSON messages, as sent)
	std::vector<LinkRec> links;
	bool link_open = false;
	std::string panel_last;
	// The CPU temperature (the Pi gets hot: owner, 2026-11-12), read by its own thread every 2 s
	// off the core's loop (a file read may block), in milli-degrees C; INT_MIN: not available.
	// Every panel message carries it as "cpu_temp_c" (null when not available).
	std::atomic<int> temp_mc{INT_MIN};
	std::atomic<bool> temp_stop{false};
	std::thread temp_thr;
	int temp_sent = INT_MIN;
	bool temp_sent_once = false;
	std::string settings_last;
	double settings_t = 0;
	wfall::Waterfall wf{10};
	int wf_n = 0;
	double tone_lo = 1400, tone_hi = 1600, afc = 0;
	bool ptt = false;
	std::string gui_dir;
	// The IC-7300 set-up (src/radio/ic7300.hpp), run by a thread of its own (CI-V takes seconds): its
	// messages go out through rig_out, taken by pump(); the settings it leaves for the settings file are
	// written by pump() (the core's thread) when it is done.
	std::thread rig_thr;
	std::atomic<bool> rig_busy{false}, rig_done{false};
	bool rig_held = false;
	std::mutex rig_mu;
	std::deque<std::string> rig_out;
	std::vector<std::pair<std::string, std::string>> rig_settings;   // for the settings file, once done
	std::string rig_done_msg;
	double rig_watts = 0;                     // ic7300_power: the wanted P2/P3 peak
	double rig_gain = 0;                      // and the TX gain it chose (rig_gain_set: to apply)
	bool rig_gain_set = false;
	radio::ic7300::power::Drive rig_drive;    // and the live drive (TX gain, PSKAmpl)

	bool start(Core &c, std::string &err)
	{
		core = &c;
		gui_dir = find_gui();
		if (!ws.start(opt.addr, opt.port, opt.web_auth, opt.ws_auth, gui_dir, err)) return false;
		const std::string host = opt.addr == "0.0.0.0" ? "127.0.0.1" : opt.addr;
		std::fprintf(stderr, "[ws] GUI http://%s:%d/  WebSocket ws://%s:%d/  command shell ws://%s:%d/shell\n",
		             host.c_str(), opt.port, host.c_str(), opt.port, host.c_str(), opt.port);
		if (gui_dir.empty()) std::fprintf(stderr, "[ws] no GUI directory found (www/index.html): give --gui DIR; the WebSocket works without it\n");
		if (opt.addr != "127.0.0.1" && opt.addr.compare(0, 4, "127.") != 0) {
			if (opt.ws_auth.empty() || opt.web_auth.empty())
				std::fprintf(stderr, "[ws] NOTE: listening on %s%s: anyone who can reach this machine can control the TNC and key the radio\n",
				             opt.addr.c_str(), opt.ws_auth.empty() ? " without --ws-auth" : " without --web-auth");
		}
		c.on_panel([this](const std::string &j) { panel(j); });
		temp_thr = std::thread([this] { temp_loop(); });
		c.on_rx([this](const std::string &t) { rx(t); });
		c.on_link([this](bool up, const std::string &call) { link(up, call); });
		c.on_log([this](const std::string &l) { log(l); });
		c.on_crypto([this]() { ws.post("crypto", "{\"type\":\"crypto\",\"crypto\":" + core->crypto_status() + "}"); ws.post("config", startup_msg()); });
		c.on_monitor([this](const std::string &who, const std::string &text) {
			ws.post("monitor", "{\"type\":\"monitor\",\"t\":" + num(now_s()) + ",\"who\":" + jsonm::quote(who) + ",\"text\":" + jsonm::quote(text) + "}");
		});
		c.on_audio([this](const int16_t *rxs, const int16_t *, size_t n) { audio(rxs, n); });
		return true;
	}

	void stop()
	{
		if (core) for (auto &kv : cl) if (kv.second.core_id > 0) core->detach(kv.second.core_id);
		cl.clear();
		ws.stop();
		temp_stop = true;
		if (temp_thr.joinable()) temp_thr.join();
		if (rig_thr.joinable()) rig_thr.join();
		if (rig_held) core->hold_ptt(false);
	}

	void pump()
	{
		std::vector<wss::Inbound> in;
		ws.take(in);
		for (auto &m : in) handle(m);
		// the stream clients' output: raw (shell) or as term messages (the web terminal)
		for (auto &kv : cl) {
			Client &c = kv.second;
			if (c.core_id <= 0) continue;
			std::string out = core->take_output(c.core_id);
			if (out.empty()) continue;
			if (c.shell) ws.post_bytes(kv.first, out);
			else ws.post("", "{\"type\":\"term\",\"data\":" + jsonm::quote(out) + "}", kv.first);
		}
		// a new CPU temperature: the panel again, with it (0.1 degree steps)
		if (const int tm = temp_mc.load(); !panel_last.empty() && (!temp_sent_once || tm / 100 != temp_sent / 100)) {
			temp_sent = tm; temp_sent_once = true;
			ws.post("panel", "{\"type\":\"panel\",\"panel\":" + with_temp(panel_last) + "}");
		}
		rig_pump();
		// the settings, as the Configure tab shows them: noticed within a second whoever changed them
		const double t = now_s();
		if (t - settings_t >= 1.0) {
			settings_t = t;
			check_settings(false);
		}
		ws.commit();
	}

private:
	static std::string num(double v) { char b[32]; std::snprintf(b, sizeof b, "%.3f", v); return b; }

	// ---- the CPU temperature (sysfs thermal zones) ----
	// The zone: the first whose type names the CPU package/SoC (x86_pkg_temp, cpu-thermal,
	// cpu_thermal, soc_thermal, acpitz), else the first readable one (a Pi's thermal_zone0 is the SoC).
	static std::string temp_zone()
	{
		static const char *const want[] = {"x86_pkg_temp", "cpu-thermal", "cpu_thermal", "soc_thermal", "acpitz"};
		std::string first;
		for (const char *w : want)
			for (int z = 0; z < 32; z++) {
				const std::string d = "/sys/class/thermal/thermal_zone" + std::to_string(z);
				FILE *f = std::fopen((d + "/type").c_str(), "r");
				if (!f) continue;
				char t[64] = {0};
				const bool ok = std::fgets(t, sizeof t, f) != nullptr;
				std::fclose(f);
				if (first.empty() && read_mc(d + "/temp") != INT_MIN) first = d + "/temp";
				if (ok && std::strncmp(t, w, std::strlen(w)) == 0 && read_mc(d + "/temp") != INT_MIN) return d + "/temp";
			}
		return first;
	}
	static int read_mc(const std::string &path)
	{
		FILE *f = std::fopen(path.c_str(), "r");
		if (!f) return INT_MIN;
		long v = 0;
		const bool ok = std::fscanf(f, "%ld", &v) == 1;
		std::fclose(f);
		return ok && v > -50000 && v < 200000 ? int(v) : INT_MIN;
	}
	// A multi-core package sensor (x86_pkg_temp) reports the hottest core at that instant: on a laptop
	// under bursty load it swings 20 °C within a second (measured 64-85 °C over 10 s). That one is
	// sampled every 0.5 s and shown as the mean of the last 10 s. A single SoC sensor (a Pi's) is
	// steady and shown as read. Published every 2 s.
	void temp_loop()
	{
		std::string path = temp_zone();
		bool smooth = false;
		if (!path.empty()) {
			FILE *f = std::fopen((path.substr(0, path.size() - 4) + "type").c_str(), "r");
			char t[64] = {0};
			if (f) { smooth = std::fgets(t, sizeof t, f) && std::strncmp(t, "x86_pkg_temp", 12) == 0; std::fclose(f); }
		}
		enum { N = 20 };
		int ring[N];
		int n = 0, at = 0;
		for (int k = 0; !temp_stop; k++) {
			if (k % 5 == 0) {
				const int v = path.empty() ? INT_MIN : read_mc(path);
				if (v == INT_MIN) n = 0;
				else {
					ring[at] = v;
					at = smooth ? (at + 1) % N : 0;
					if (n < (smooth ? int(N) : 1)) n++;
				}
				if (k % 20 == 0) {
					long s = 0;
					for (int i = 0; i < n; i++) s += ring[i];
					temp_mc = n ? int(s / n) : INT_MIN;
				}
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
	}
	std::string with_temp(const std::string &j) const
	{
		if (j.empty() || j.back() != '}') return j;
		const int tm = temp_mc.load();
		char b[48];
		if (tm == INT_MIN) std::snprintf(b, sizeof b, ",\"cpu_temp_c\":null}");
		else std::snprintf(b, sizeof b, ",\"cpu_temp_c\":%.1f}", tm / 1000.0);
		return j.substr(0, j.size() - 1) + b;
	}

	// ---- events from the core ----
	void panel(const std::string &j)
	{
		panel_last = j;
		while (!panel_last.empty() && (panel_last.back() == '\n' || panel_last.back() == '\r')) panel_last.pop_back();
		ws.post("panel", "{\"type\":\"panel\",\"panel\":" + with_temp(panel_last) + "}");
		jsonm::Value v;
		if (jsonm::parse(panel_last, v)) {
			if (const jsonm::Value *d = v.get("detail")) {
				afc = d->num("afc_hz", 0);
				if (const jsonm::Value *p = d->get("ptt")) ptt = p->kind == jsonm::Value::BOOL && p->b;
				if (link_open && !links.empty()) {
					LinkRec &r = links.back();
					const int pac = int(d->num("plevel", 0)), lv = int(d->num("speed", 0));
					if (pac) r.plevel = pac;
					if (lv) { r.level_last = lv; if (lv > r.level_max) r.level_max = lv; }
					r.rx_bytes = long(d->num("rx_bytes", double(r.rx_bytes)));
					r.tx_bytes = long(d->num("tx_text", double(r.tx_bytes)));
					const std::string role = d->str("role");
					if (role == "caller" || role == "callee") r.role = role;
				}
			}
		}
	}
	void rx(const std::string &t)
	{
		rx_hist += t;
		if (rx_hist.size() > 4096) rx_hist.erase(0, rx_hist.size() - 4096);
		ws.post("rx", "{\"type\":\"rx\",\"text\":" + jsonm::quote(t) + "}");
	}
	void link(bool up, const std::string &call)
	{
		// the link's details (role, level, why it ended) beside up/call, when the core gives them
		std::string lj = core->link_json(), extra;
		if (lj.size() > 2) extra = ",\"link\":" + lj;
		ws.post("link", "{\"type\":\"link\",\"up\":" + std::string(up ? "true" : "false") + ",\"call\":" + jsonm::quote(call) + extra + "}");
		const long long unix_now = (long long)std::time(nullptr);
		if (up) {
			LinkRec r; r.start = now_s(); r.unix_start = unix_now; r.call = call;
			links.push_back(r);
			if (links.size() > 1000) links.erase(links.begin());
			link_open = true;
		} else if (link_open && !links.empty()) {
			links.back().end = now_s(); links.back().unix_end = unix_now;
			link_open = false;
			ws.post("link", "{\"type\":\"link_logged\",\"call\":" + jsonm::quote(links.back().call) + "}");
		}
	}
	void log(const std::string &l)
	{
		std::string s = l;
		while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
		const std::string j = "{\"type\":\"log\",\"t\":" + num(now_s()) + ",\"text\":" + jsonm::quote(s) + "}";
		log_ring.push_back(j);
		if (log_ring.size() > 500) log_ring.pop_front();
		ws.post("log", j);
	}
	void audio(const int16_t *rxs, size_t n)
	{
		if (wf_n <= 0 || !rxs) return;
		for (size_t i = 0; i < n; i++) {
			if (!wf.put(rxs[i])) continue;
			char hd[200];
			std::snprintf(hd, sizeof hd, "{\"type\":\"waterfall\",\"t\":%.2f,\"tx\":%s,\"lo\":%.0f,\"hi\":%.0f,\"afc\":%.1f,\"row\":\"",
			              now_s(), ptt ? "true" : "false", tone_lo, tone_hi, afc);
			const std::string j = hd + wfall::Waterfall::base64(wf.row(), wfall::Waterfall::kBins) + "\"}";
			for (auto &kv : cl) if (kv.second.wf) ws.post("", j, kv.first);
		}
	}

	// ---- settings ----
	std::string config_msg(const std::string &settings) const
	{
		return "{\"type\":\"config\",\"persist\":false,\"profile\":\"\",\"profiles\":[],\"station\":" + (settings.empty() ? std::string("{}") : settings) + "}";
	}
	void check_settings(bool force)
	{
		const std::string s = core->settings_json();
		if (!force && s == settings_last) return;
		settings_last = s;
		jsonm::Value v;
		if (jsonm::parse(s, v)) { tone_lo = v.num("tone_lo", tone_lo); tone_hi = v.num("tone_hi", tone_hi); }
		ws.post("config", config_msg(s));
	}
	static std::string value_text(const jsonm::Value &v)
	{
		switch (v.kind) {
		case jsonm::Value::STR: return v.s;
		case jsonm::Value::NUM: { char b[32]; std::snprintf(b, sizeof b, "%.10g", v.n); return b; }
		case jsonm::Value::BOOL: return v.b ? "1" : "0";
		default: return "";
		}
	}
	std::string links_json(int lim) const
	{
		std::string j = "[";
		int k = 0;
		for (auto it = links.rbegin(); it != links.rend() && k < lim; ++it, ++k) {
			const LinkRec &r = *it;
			const long long end = r.unix_end ? r.unix_end : (long long)std::time(nullptr);
			char b[512];
			std::snprintf(b, sizeof b, "%s{\"id\":%d,\"start\":%lld,\"end\":%lld,\"open\":%s,\"call\":%s,\"role\":%s,\"modem\":\"signum\",\"plevel\":%d,"
			              "\"level_max\":%d,\"level_last\":%d,\"rx_bytes\":%ld,\"tx_bytes\":%ld}",
			              k ? "," : "", int(links.size()) - k, r.unix_start, end, r.unix_end ? "false" : "true", jsonm::quote(r.call).c_str(),
			              jsonm::quote(r.role).c_str(), r.plevel, r.level_max, r.level_last, r.rx_bytes, r.tx_bytes);
			j += b;
		}
		return j + "]";
	}

	// ---- the IC-7300 set-up -------------------------------------------------------------------------
	void rig_post(const std::string &j) { std::lock_guard<std::mutex> g(rig_mu); rig_out.push_back(j); }
	static std::string rig_plan_json(const radio::ic7300::Plan &p, const std::string &err, const std::vector<std::string> &users)
	{
		namespace ic = radio::ic7300;
		std::string j = "{\"serial\":" + jsonm::quote(p.radio.serial) + ",\"port\":" + jsonm::quote(p.radio.port) + ",\"dev\":" + jsonm::quote(p.radio.dev) +
		                ",\"alsa\":" + jsonm::quote(p.radio.alsa) + ",\"card\":" + jsonm::quote(p.radio.card) + ",\"answered\":" + (p.probe.answered ? "true" : "false") +
		                ",\"addr\":" + std::to_string(p.probe.addr) + ",\"baud\":" + std::to_string(p.probe.baud) + ",\"how\":" + jsonm::quote(p.probe.how) +
		                ",\"needs_b0_exit\":" + (p.probe.needs_b0_exit && !p.probe.answered ? "true" : "false") + ",\"error\":" + jsonm::quote(err) +
		                ",\"changes\":" + std::to_string(p.changes()) + ",\"profile\":" + jsonm::quote(p.profile == ic::Profile::EXTERNAL ? "external" : "signum") + ",\"users\":[";
		for (size_t k = 0; k < users.size(); k++) j += (k ? "," : "") + jsonm::quote(users[k]);
		j += "],\"warning\":" + jsonm::quote(p.radio.serial.empty() ? "" : ic::warning_text(p, ic::config_dir())) + ",\"settings\":[";
		bool first = true;
		if (p.profile == ic::Profile::SIGNUM) for (const auto &kv : ic::signum_settings(p.radio)) { j += std::string(first ? "" : ",") + "[" + jsonm::quote(kv.first) + "," + jsonm::quote(kv.second) + "]"; first = false; }
		j += "],\"rows\":[";
		for (size_t k = 0; k < p.rows.size(); k++) {
			const auto &r = p.rows[k];
			j += std::string(k ? "," : "") + "{\"key\":" + jsonm::quote(r.it->key) + ",\"name\":" + jsonm::quote(r.it->name) + ",\"now\":" + jsonm::quote(r.now_s) +
			     ",\"new\":" + jsonm::quote(r.want.empty() ? std::string(r.it->target.empty() ? "(recorded only)" : "(left as it is)") : r.new_s) + ",\"change\":" + (r.change ? "true" : "false") + "}";
		}
		return j + "]}";
	}
	static std::vector<std::string> port_users(const std::string &dev);
	// One operation: "detect" (every radio, read only), "backup", "setup" or "restore" (one radio).
	void rig_start(const std::string &op, const std::string &serial, const std::string &file, bool allow_b0_exit, bool external = false)
	{
		if (rig_thr.joinable()) rig_thr.join();
		rig_busy = true; rig_done = false;
		radio::ic7300::power::busy() = true;     // the console's ICPower waits too
		rig_settings.clear(); rig_done_msg.clear();
		if (!rig_held) { core->hold_ptt(true); rig_held = true; }   // every operation: opening the port clears RTS/DTR
		rig_thr = std::thread([this, op, serial, file, allow_b0_exit, external]() {
			namespace ic = radio::ic7300;
			const ic::Profile profile = external ? ic::Profile::EXTERNAL : ic::Profile::SIGNUM;
			const ic::Say say = [this](const std::string &stage, const std::string &text, int ok) {
				rig_post("{\"type\":\"ic7300\",\"event\":\"step\",\"stage\":" + jsonm::quote(stage) + ",\"text\":" + jsonm::quote(text) + ",\"ok\":" + std::to_string(ok) + "}");
			};
			auto done = [this, &op, external](bool ok, const std::string &extra) {
				rig_done_msg = "{\"type\":\"ic7300\",\"event\":\"done\",\"op\":" + jsonm::quote(op) + ",\"profile\":" + jsonm::quote(external ? "external" : "signum") + ",\"ok\":" + (ok ? "true" : "false") + extra + "}";
				rig_done = true;
			};
			const char *root = std::getenv("SIGNUM_TEST_IC7300_ROOT");   // tests: a fake /dev + /sys tree
			const auto all = ic::find(root ? root : "");
			if (op == "power") {                         // RF POWER for a wanted P2/P3 peak (the model; never transmits)
				const ic::Radio *r = nullptr;
				for (const auto &x : all) if (serial.empty() ? all.size() == 1 : x.serial == serial) r = &x;
				if (!r) return done(false, ",\"error\":" + jsonm::quote(all.empty() ? "no IC-7300 found" : serial.empty() ? "more than one IC-7300: choose one (serial)" : "no IC-7300 with serial number " + serial + " here"));
				util::SafePort port;
				const ic::power::Result pr = ic::power::set_power(port, *r, rig_watts, rig_drive, nullptr, say);
				if (pr.ok) { rig_gain = pr.gain_after; rig_gain_set = true; }   // applied in rig_pump, on the loop's thread
				std::string ls = ",\"lines\":[";
				for (size_t k = 0; k < pr.lines.size(); k++) ls += (k ? "," : "") + jsonm::quote(pr.lines[k]);
				return done(pr.ok, ls + "],\"before\":" + std::to_string(pr.before) + ",\"after\":" + std::to_string(pr.after) + ",\"backup\":" + jsonm::quote(pr.backup) + ",\"error\":" + jsonm::quote(pr.error));
			}
			if (op == "detect") {
				std::string j = "{\"type\":\"ic7300\",\"event\":\"detect\",\"radios\":[";
				for (size_t k = 0; k < all.size(); k++) {
					util::SafePort port;
					ic::Plan plan; std::string err;
					ic::examine(port, all[k], plan, false, err, say, profile);
					plan.radio = all[k];
					j += (k ? "," : "") + rig_plan_json(plan, err, port_users(all[k].dev));
				}
				rig_post(j + "]}");
				return done(true, ",\"count\":" + std::to_string(all.size()));
			}
			ic::Backup bk;
			std::string err, want = serial;
			if (op == "restore") {
				if (!ic::read_backup(file, bk, err)) return done(false, ",\"error\":" + jsonm::quote(err));
				if (want.empty()) want = bk.serial;
			}
			const ic::Radio *r = nullptr;
			for (const auto &x : all) if (x.serial == want) r = &x;
			if (!r) return done(false, ",\"error\":" + jsonm::quote(all.empty() ? "no IC-7300 found" : "no IC-7300 with serial number " + want + " here"));
			util::SafePort port;
			ic::Plan plan;
			if (!ic::examine(port, *r, plan, allow_b0_exit, err, say, profile)) return done(false, ",\"error\":" + jsonm::quote(err));
			if (op == "backup") {
				const std::string path = ic::backup_path(ic::config_dir(), r->serial);
				const std::string why = ic::write_backup(plan, path, "a backup on request (the GUI)");
				return done(why.empty(), ",\"backup\":" + jsonm::quote(path) + ",\"error\":" + jsonm::quote(why));
			}
			const ic::Result res = op == "setup" ? ic::setup(port, plan, say) : ic::restore(port, plan, bk, say);
			if (op == "setup" && res.ok && !external) rig_settings = ic::signum_settings(*r);
			done(res.ok, ",\"written\":" + std::to_string(res.written) + ",\"verified\":" + std::to_string(res.verified) + ",\"failed\":" + std::to_string(res.failed) +
			             ",\"backup\":" + jsonm::quote(res.backup) + ",\"error\":" + jsonm::quote(res.error));
		});
	}
	void rig_pump()
	{
		std::deque<std::string> q;
		{ std::lock_guard<std::mutex> g(rig_mu); q.swap(rig_out); }
		for (const auto &j : q) ws.post("config", j);
		if (!rig_done) return;
		rig_done = false;
		if (rig_thr.joinable()) rig_thr.join();
		std::string msg = rig_done_msg;
		if (!rig_settings.empty()) {                    // the settings file, on this thread
			std::string w = ",\"settings\":[";
			for (size_t k = 0; k < rig_settings.size(); k++) {
				const std::string why = core->startup_set(rig_settings[k].first, rig_settings[k].second);
				w += std::string(k ? "," : "") + "[" + jsonm::quote(rig_settings[k].first) + "," + jsonm::quote(rig_settings[k].second) + "," + jsonm::quote(why) + "]";
			}
			msg = msg.substr(0, msg.size() - 1) + w + "]}";
			rig_settings.clear();
			ws.post("config", startup_msg());
		}
		if (rig_gain_set) {                             // ic7300_power's drive: live now and in the settings file
			rig_gain_set = false;
			char v[24];
			std::snprintf(v, sizeof v, "%g", rig_gain);
			jsonm::Value sv;
			const double was = jsonm::parse(core->settings_json(), sv) ? sv.num("tx_gain", 0) : 0;
			if (std::fabs(was - rig_gain) > 1e-6) core->set("tx_gain", v);
			const std::string why = core->startup_set("tx-gain", v);
			char t[64];
			std::snprintf(t, sizeof t, ",\"tx_gain_before\":%.1f,\"tx_gain\":%.1f", was, rig_gain);
			msg = msg.substr(0, msg.size() - 1) + t + ",\"tx_gain_saved\":" + jsonm::quote(why) + "}";
			ws.post("config", startup_msg());
			check_settings(true);
		}
		if (rig_held) { core->hold_ptt(false); rig_held = false; }
		rig_busy = false;
		radio::ic7300::power::busy() = false;
		ws.post("config", msg);
	}
	std::string rig_backups_json() const
	{
		std::string j = "{\"type\":\"ic7300\",\"event\":\"backups\",\"dir\":" + jsonm::quote(radio::ic7300::config_dir()) + ",\"files\":[";
		const auto v = radio::ic7300::backups();
		for (size_t k = 0; k < v.size(); k++) {
			radio::ic7300::Backup b; std::string e;
			const bool ok = radio::ic7300::read_backup(v[k], b, e);
			j += std::string(k ? "," : "") + "{\"path\":" + jsonm::quote(v[k]) + ",\"serial\":" + jsonm::quote(ok ? b.serial : "") + ",\"items\":" + std::to_string(b.values.size()) + ",\"rf_power\":" + (b.rf_power ? "true" : "false") + "}";
		}
		return j + "]}";
	}

	// ---- one message from a client ----
	void detach_core(Client &c)
	{
		if (c.core_id > 0) core->detach(c.core_id);
		c.core_id = 0;
	}
	void hello(int id)
	{
		auto send = [&](const std::string &j) { ws.post("", j, id); };
		// "crypto": false in a build without encryption (the default): the GUI shows no encryption control at all.
		const bool cr = core->crypto_built();
		send("{\"type\":\"hello\",\"proto\":1,\"server\":" + jsonm::quote(core->version()) + ",\"mycall\":" + jsonm::quote(core->mycall()) +
		     ",\"streams\":[\"panel\",\"rx\",\"link\",\"log\",\"event\",\"config\",\"monitor\"" + (cr ? ",\"crypto\"" : "") + "],\"subscribed\":[\"panel\",\"rx\",\"link\",\"event\",\"config\",\"monitor\"" + (cr ? ",\"crypto\"" : "") + "],"
		     "\"requests\":[\"command\",\"connect\",\"disconnect\",\"abort\",\"send\",\"changeover\",\"link_status\",\"subscribe\",\"get\",\"ping\",\"auth\",\"config_get\",\"config_set\","
		     "\"startup_get\",\"startup_set\",\"term_open\",\"term\",\"term_close\",\"waterfall\",\"links\",\"log_get\",\"about\",\"ic7300_detect\",\"ic7300_backup\",\"ic7300_setup\",\"ic7300_restore\",\"ic7300_backups\",\"ic7300_power\"" + (cr ? ",\"crypto\"" : "") + "],\"shell\":\"/shell\",\"power\":false,\"crypto\":" + (cr ? "true" : "false") + "}");
		if (!panel_last.empty() || !core->panel_json().empty())
			send("{\"type\":\"panel\",\"panel\":" + with_temp(panel_last.empty() ? core->panel_json() : panel_last) + "}");
		if (!rx_hist.empty()) send("{\"type\":\"rx\",\"backlog\":true,\"text\":" + jsonm::quote(rx_hist) + "}");
		settings_last = core->settings_json();
		send(config_msg(settings_last));
		send(startup_msg());
		if (cr) send("{\"type\":\"crypto\",\"crypto\":" + core->crypto_status() + "}");
	}
	// The settings file: what applies at the next start (src/start_config.hpp).
	std::string startup_msg() const { return "{\"type\":\"startup\",\"startup\":" + core->startup_json() + "}"; }
	void handle(const wss::Inbound &m)
	{
		if (m.event == 2) {
			auto it = cl.find(m.client);
			if (it != cl.end()) {
				if (it->second.wf) wf_n--;
				detach_core(it->second);
				cl.erase(it);
			}
			return;
		}
		if (m.event == 1) {
			Client &c = cl[m.client];
			c.authed = m.text == "authed";
			c.shell = m.path.compare(0, 6, "/shell") == 0;
			if (c.shell) {
				// The command shell: credentials only in the request (Basic header or ?auth=), there is no
				// JSON to carry an auth message.
				if (!c.authed) {
					ws.post_bytes(m.client, "*** authorization required (--ws-auth): Basic header or ?auth=BASE64(USER:PASS)\r\n");
					ws.close_client(m.client, true);
					return;
				}
				c.core_id = core->attach("ws-shell-" + std::to_string(m.client));
				if (c.core_id <= 0) { ws.post_bytes(m.client, "*** the TNC refused another client\r\n"); ws.close_client(m.client, true); }
				return;
			}
			if (!c.authed) { ws.post("", "{\"type\":\"hello\",\"proto\":1,\"auth\":\"required\"}", m.client); return; }
			hello(m.client);
			return;
		}
		auto it = cl.find(m.client);
		if (it == cl.end()) return;
		Client &c = it->second;
		if (c.shell) {                                  // raw bytes for the interpreter
			if (c.core_id <= 0) return;
			const std::string b = m.binary ? m.text : utf8_to_latin1(m.text);
			core->input(c.core_id, reinterpret_cast<const uint8_t *>(b.data()), b.size());
			return;
		}
		auto send = [&](const std::string &j) { ws.post("", j, m.client); };
		jsonm::Value v;
		if (!jsonm::parse(m.text, v) || v.kind != jsonm::Value::OBJ) { send("{\"type\":\"error\",\"error\":\"not a JSON object\"}"); return; }
		const std::string type = v.str("type");
		std::string id = "null";
		if (const jsonm::Value *iv = v.get("id")) {
			if (iv->kind == jsonm::Value::NUM) id = std::to_string((long long)iv->n);
			else if (iv->kind == jsonm::Value::STR) id = jsonm::quote(iv->s);
		}
		auto result = [&](const std::string &text, bool ok = true) {
			send("{\"type\":\"result\",\"id\":" + id + ",\"ok\":" + (ok ? "true" : "false") + ",\"text\":" + jsonm::quote(text) + "}");
		};
		if (!c.authed) {
			if (type == "auth" && !opt.ws_auth.empty() && v.str("user") + ":" + v.str("pass") == opt.ws_auth) {
				c.authed = true;
				result("ok");
				hello(m.client);
			} else send("{\"type\":\"error\",\"id\":" + id + ",\"error\":\"auth required\"}");
			return;
		}
		if (type == "term") {
			if (c.core_id > 0) { const std::string b = utf8_to_latin1(v.str("data")); core->input(c.core_id, reinterpret_cast<const uint8_t *>(b.data()), b.size()); }
			return;
		}
		if (type == "term_open") {
			if (c.core_id <= 0) c.core_id = core->attach("ws-term-" + std::to_string(m.client));
			if (c.core_id <= 0) { result("the TNC refused another client", false); return; }
			send("{\"type\":\"term\",\"data\":" + jsonm::quote("\r\n*** " + core->mycall() + " terminal\r\n") + "}");
			core->input(c.core_id, reinterpret_cast<const uint8_t *>("\r"), 1);   // a prompt
			return;
		}
		if (type == "term_close") { detach_core(c); result(""); return; }
		if (type == "command") {
			const std::string line = v.str("line");
			const std::string r = core->command(line);
			result(r, r.find("ERROR") == std::string::npos);   // ok: false on an ERROR answer
			check_settings(false);
			return;
		}
		if (type == "connect") {
			const std::string call = v.str("call");
			if (call.empty()) { result("no call", false); return; }
			core->connect(call);
			result("calling " + call);
			return;
		}
		if (type == "disconnect") { core->disconnect(); result("disconnecting"); return; }
		if (type == "abort") { core->abort(); result("aborted"); return; }
		if (type == "send") { core->send(utf8_to_latin1(v.str("text"))); result(""); return; }
		if (type == "ping") { send("{\"type\":\"pong\",\"id\":" + id + "}"); return; }
		if (type == "changeover") {                       // the host's Ctrl-Y (%O), or with break_in_only a break-in (%I)
			const jsonm::Value *b = v.get("break_in_only");
			const bool ok = core->changeover(b && b->kind == jsonm::Value::BOOL && b->b);
			result(ok ? "ok" : "nothing to do now", ok);
			return;
		}
		if (type == "link_status") {                      // the link now: state, counters, why the last one ended
			const std::string lj = core->link_json();
			send("{\"type\":\"link_status\",\"id\":" + id + ",\"link\":" + (lj.empty() ? std::string("null") : lj) + "}");
			return;
		}
		if (type == "crypto") {                           // encryption (docs/md/encryption.md); refused in a build without encryption
			if (!core->crypto_built()) { send("{\"type\":\"error\",\"id\":" + id + ",\"error\":\"not built with encryption (./configure --enable-encryption)\"}"); return; }
			send("{\"type\":\"crypto_result\",\"id\":" + id + "," + core->crypto_request(m.text) + "}");
			ws.post("config", startup_msg());             // the acknowledgement (and encryption = on) live in the settings file
			return;
		}
		if (type == "get") {
			const std::string p = panel_last.empty() ? core->panel_json() : panel_last;
			send("{\"type\":\"panel\",\"id\":" + id + ",\"panel\":" + (p.empty() ? std::string("null") : with_temp(p)) + "}");
			return;
		}
		if (type == "subscribe") {
			std::vector<std::string> s;
			if (const jsonm::Value *a = v.get("streams")) for (const auto &x : a->a) if (x.kind == jsonm::Value::STR) s.push_back(x.s);
			ws.subscribe(m.client, s);
			result("ok");
			return;
		}
		if (type == "config_get" || type == "profiles") { check_settings(false); send(config_msg(settings_last)); return; }
		if (type == "config_set") {
			std::string out;
			bool ok = true;
			if (const jsonm::Value *st = v.get("station")) {
				for (const auto &kv : st->o) {
					const std::string r = core->set(kv.first, value_text(kv.second));
					if (r.find("ERROR") != std::string::npos) ok = false;
					out += r;
					if (!r.empty() && r.back() != '\n') out += "\r\n";
				}
			}
			result(out, ok);
			check_settings(true);
			return;
		}
		if (type == "startup_get") { send(startup_msg()); return; }
		if (type == "startup_set") {                    // {"options": {option: value, or null / "" to remove}}
			std::string out;
			bool ok = true;
			if (const jsonm::Value *o = v.get("options")) {
				for (const auto &kv : o->o) {
					const std::string val = kv.second.kind == jsonm::Value::NUL ? std::string() : value_text(kv.second);
					const std::string r = core->startup_set(kv.first, kv.second.kind == jsonm::Value::BOOL ? (kv.second.b ? "on" : "off") : val);
					if (!r.empty()) { ok = false; out += r + "\r\n"; }
				}
			}
			result(ok ? "saved for the next start" : out, ok);
			ws.post("config", startup_msg());
			return;
		}
		if (type.compare(0, 7, "ic7300_") == 0) {         // the IC-7300 set-up (src/radio/ic7300.hpp)
			const bool understood = [&] { const jsonm::Value *u = v.get("understood"); return u && u->kind == jsonm::Value::BOOL && u->b; }();
			const bool b0 = [&] { const jsonm::Value *u = v.get("allow_b0_exit"); return u && u->kind == jsonm::Value::BOOL && u->b; }();
			if (type == "ic7300_backups") { send(rig_backups_json()); return; }
			if (type != "ic7300_detect" && type != "ic7300_backup" && type != "ic7300_setup" && type != "ic7300_restore" && type != "ic7300_power") { result("unknown IC-7300 request " + type, false); return; }
			if (rig_busy) { result("the IC-7300 set-up is busy with the radio: wait for it to finish", false); return; }
			if (radio::ic7300::power::busy()) { result("the radio is busy (the console's ICPower): wait for it to finish", false); return; }
			if (type == "ic7300_power") {                 // RF POWER for a wanted P2/P3 peak: the model, never transmits
				const double w = v.num("watts", -1);
				if (!(w > 0)) { result("how many watts? (watts: the wanted P2/P3 peak power)", false); return; }
				if (!core->quiet()) { result("not while a link is up or the station is transmitting: disconnect first", false); return; }
				jsonm::Value sv;
				rig_drive = radio::ic7300::power::Drive();
				if (jsonm::parse(core->settings_json(), sv)) { rig_drive.tx_gain_db = sv.num("tx_gain", 0); rig_drive.pska = int(sv.num("pska", 140)); }
				rig_watts = w;
				radio::ic7300::power::busy() = true;
				rig_start("power", v.str("serial"), "", false);
				result("power started");
				return;
			}
			const std::string op = type.substr(7);
			if ((op == "setup" || op == "restore") && !understood) { result("not confirmed: tick \"I understand my radio's current configuration will be lost\" first (understood: true)", false); return; }
			if ((op == "setup" || op == "restore" || op == "backup") && v.str("serial").empty() && op != "restore") { result("which radio? (serial)", false); return; }
			if (op == "restore" && v.str("file").empty()) { result("which backup file? (file)", false); return; }
			// even a read opens the port with RTS/DTR cleared (the lines are the tty's, shared with our PTT): never during a link
			if (!core->quiet()) { result("not while a link is up or the station is transmitting: disconnect first", false); return; }
			rig_start(op, v.str("serial"), v.str("file"), b0, v.str("profile") == "external");
			result(op == "detect" ? "looking for IC-7300s" : op + " started");
			return;
		}
		if (type == "waterfall") {
			const jsonm::Value *on = v.get("on");
			const bool want = !(on && on->kind == jsonm::Value::BOOL && !on->b);
			if (want != c.wf) {
				if (want && wf_n == 0) wf.reset();
				c.wf = want;
				wf_n += want ? 1 : -1;
			}
			if (const jsonm::Value *r = v.get("rate")) if (r->kind == jsonm::Value::NUM) wf.set_rate(int(r->n));
			char hb[200];
			std::snprintf(hb, sizeof hb, ",\"on\":%s,\"rate\":%d,\"bin_hz\":%.3f,\"bins\":%d,\"db0\":-127.5,\"db_step\":0.5}",
			              c.wf ? "true" : "false", wf.rate(), wfall::Waterfall::kBinHz, wfall::Waterfall::kBins);
			send("{\"type\":\"waterfall_info\",\"id\":" + id + hb);
			return;
		}
		if (type == "links") {
			const jsonm::Value *l = v.get("limit");
			const int lim = l && l->kind == jsonm::Value::NUM ? std::max(1, std::min(1000, int(l->n))) : 50;
			send("{\"type\":\"links\",\"id\":" + id + ",\"kept\":true,\"links\":" + links_json(lim) + "}");
			return;
		}
		if (type == "log_get") {                        // the log kept so far (the last 500 lines)
			for (const auto &j : log_ring) send(j);
			result(std::to_string(log_ring.size()) + " lines");
			return;
		}
		if (type == "about") {
			send("{\"type\":\"about\",\"id\":" + id + ",\"version\":" + jsonm::quote(core->version()) + ",\"built\":" + jsonm::quote(__DATE__ " " __TIME__) + "}");
			return;
		}
		send("{\"type\":\"error\",\"id\":" + id + ",\"error\":" + jsonm::quote("unknown request type: " + type) + "}");
	}
};

std::vector<std::string> Frontend::port_users(const std::string &dev)
{
	std::vector<std::string> v;
	const std::string me = std::to_string(::getpid());
	if (DIR *d = ::opendir("/proc")) {
		while (dirent *e = ::readdir(d)) {
			if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
			const std::string fdd = std::string("/proc/") + e->d_name + "/fd";
			if (DIR *f = ::opendir(fdd.c_str())) {
				while (dirent *x = ::readdir(f)) {
					char t[PATH_MAX];
					const ssize_t k = ::readlink((fdd + "/" + x->d_name).c_str(), t, sizeof t - 1);
					if (k > 0 && std::string(t, size_t(k)) == dev) {
						v.push_back(std::string(e->d_name) + (me == e->d_name ? " (this Signum: its PTT)" : " (" + radio::ic7300::detail::slurp(std::string("/proc/") + e->d_name + "/comm") + ")"));
						break;
					}
				}
				::closedir(f);
			}
		}
		::closedir(d);
	}
	return v;
}

Frontend *g_fe = nullptr;

bool arg_value(int &i, int argc, char **argv, std::string &out)
{
	if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", argv[i]); std::exit(2); }
	out = argv[++i];
	return true;
}

}  // namespace

// ---- the hooks (tnc_core.hpp) ------------------------------------------------------------
bool frontend_arg(int &i, int argc, char **argv)
{
	const std::string a = argv[i];
	std::string v, err;
	if (a == "--no-ws") { opt.on = false; return true; }
	if (a == "--ws") {
		arg_value(i, argc, argv, v);
		const size_t c = v.rfind(':');
		if (c != std::string::npos) { opt.addr = v.substr(0, c); v = v.substr(c + 1); }
		opt.port = std::atoi(v.c_str());
		if (opt.port <= 0 || opt.port > 65535 || opt.addr.empty()) { std::fprintf(stderr, "--ws: expected [ADDR:]PORT\n"); std::exit(2); }
		return true;
	}
	if (a == "--web-auth") { arg_value(i, argc, argv, opt.web_auth); return true; }
	if (a == "--ws-auth") { arg_value(i, argc, argv, opt.ws_auth); return true; }
	if (a == "--web-auth-file" || a == "--ws-auth-file") {
		arg_value(i, argc, argv, v);
		if (!read_cred_file(v, a == "--ws-auth-file" ? opt.ws_auth : opt.web_auth, err)) { std::fprintf(stderr, "%s: %s\n", a.c_str(), err.c_str()); std::exit(2); }
		return true;
	}
	if (a == "--gui") { arg_value(i, argc, argv, opt.gui); return true; }
	return false;
}

void frontend_usage(FILE *f)
{
	std::fprintf(f,
	             "Front ends (web GUI, WebSocket API, command shell on a WebSocket; src/web/README.md):\n"
	             "  --ws [ADDR:]PORT        listen there (default 127.0.0.1:8770; 0.0.0.0 for every interface)\n"
	             "  --no-ws                 no GUI, no WebSocket\n"
	             "  --web-auth USER:PASS    a login for the GUI pages (HTTP Basic); default open\n"
	             "  --ws-auth USER:PASS     a login for the WebSocket API and /shell; default open\n"
	             "  --web-auth-file F, --ws-auth-file F   the same, from the first non-# line of a file\n"
	             "  --gui DIR               the directory served over HTTP (default: www/ beside the binary)\n");
}

bool frontend_start(Core &core, std::string &err)
{
	if (!opt.on) return true;
	g_fe = new Frontend;
	if (!g_fe->start(core, err)) { delete g_fe; g_fe = nullptr; err = "[ws] " + err; return false; }
	return true;
}

void frontend_pump(Core &)
{
	if (g_fe) g_fe->pump();
}

void frontend_stop()
{
	if (!g_fe) return;
	g_fe->stop();
	delete g_fe;
	g_fe = nullptr;
}

}  // namespace signum

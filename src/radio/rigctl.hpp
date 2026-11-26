// rigctl.hpp: rig control through Hamlib's rigctld (TODO 5, user 2026-10-20).
//
// The station is a rigctld CLIENT over TCP (no libhamlib link: the same code
// serves an ESP32 talking to a rigctld over the network). Three set-ups,
// "control" in the profile's "rig" block (src/web/README.md "Rig control"):
//   none     the station never touches the radio (the user's program owns it)
//   shared   connect to a rigctld the user runs (host:port), shared with
//            their program (Pat's hamlib setting points at the same one)
//   own      the station starts its own rigctld (model, device, baud) on
//            127.0.0.1:port and stops it on exit
// It reads frequency, mode, PTT and the S-meter every poll_ms, every other
// setting the radio reports every few polls (levels, functions, parameters,
// VFO, RIT/XIT, split, CTCSS, ...), and sets the frequency (kHz in the
// profile) and mode when they are given or changed.
// Everything runs on the client's own thread: nothing here blocks the caller.
// PTT through rigctld is src/radio/ptt.hpp's "rig" kind (its own
// connection, non-blocking), not this client.
//
// The protocol: rigctld's extended responses ("+f" -> "get_freq:",
// "Frequency: 14070000", "RPRT 0"), every answer ending with "RPRT n", n < 0
// an error (checked against rigctld -m 1, Hamlib 4.7.2, 2026-10-20).
//
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
#pragma once

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace radio {

// Open a TCP connection; timeout_ms bounds the connect. -1 on failure.
int tcp_connect(const std::string &host, int port, int timeout_ms);

// The amateur bands a per-band RF power cap can name (kHz, the widest of the
// ITU regions' allocations). A frequency outside all of them gets the
// station's overall cap only.
struct HamBand { const char *name; double lo_khz, hi_khz; };
const std::vector<HamBand> &ham_bands();
inline const char *ham_band(double khz)
{
	for (const auto &b : ham_bands()) if (khz >= b.lo_khz && khz <= b.hi_khz) return b.name;
	return "";
}

struct RigConfig {
	std::string control = "none";      // none | shared | own
	std::string host = "127.0.0.1";
	int port = 4532;
	int model = 0;                     // own: Hamlib model number (rigctl -l; IC-7300 3073)
	std::string device;                // own: the radio's serial port (CI-V)
	int baud = 0;                      // own: 0 = rigctld's default for the model
	// own: rigctld --set-conf. RTS and DTR held low by default: many radios key
	// on RTS (an IC-7300 set to USB SEND = RTS keys the moment a program opens
	// its port with RTS high, the Linux default). A cable powered from DTR/RTS
	// needs this changed. Linux still raises both lines for an instant when the
	// port is opened at a speed, or when rigctld sets its speed on a port at B0
	// (where the PTT leaves it, src/util/serial_safe.hpp), before rigctld can
	// lower them.
	std::string conf = "rts_state=OFF,dtr_state=OFF";
	double freq_khz = 0;               // set on connect and on change; 0 = leave the radio alone
	std::string mode;                  // Hamlib mode (USB, PKTUSB, ...); "" = leave
	int passband = 0;                  // with mode: Hz, 0 = the radio's default
	int poll_ms = 1000;
	// The highest RF power level the station will set (Hamlib RFPOWER, 0..1 of
	// the radio's maximum). 1 = no cap. A radio found above it is reported.
	double max_rfpower = 1.0;
	// A lower cap on some bands (ham_bands() names; 0..1 as max_rfpower). The
	// cap at a frequency is the smaller of the two.
	std::map<std::string, double> band_rfpower;
	double cap_at(double hz) const
	{
		auto it = band_rfpower.find(ham_band(hz / 1000.0));
		return it == band_rfpower.end() ? max_rfpower : std::min(max_rfpower, it->second);
	}
	bool enabled() const { return control == "shared" || control == "own"; }
	bool same_link(const RigConfig &o) const   // the connection itself (anything else applies without reconnecting)
	{
		return control == o.control && host == o.host && port == o.port && model == o.model && device == o.device && baud == o.baud && conf == o.conf;
	}
};

// Hamlib mode names rigctld accepts (rig.h rig_strrmode).
inline bool valid_mode(const std::string &m)
{
	static const char *const k[] = {"USB", "LSB", "CW", "CWR", "RTTY", "RTTYR", "AM", "FM", "WFM", "PKTUSB", "PKTLSB", "PKTFM", "PKTAM", "FMN", "AMS", "DSB", "SAM", "SAL", "SAH"};
	for (const char *x : k) if (m == x) return true;
	return false;
}

// What the radio can do (rigctld \dump_caps), learned on connect.
struct RigCaps {
	bool ok = false;
	std::vector<std::string> levels_get, levels_set, funcs_get, funcs_set, modes, vfos;
	std::map<std::string, std::array<double, 3>> range;   // level -> min, max, step
	std::vector<int> preamp, att;                          // dB choices (0 = off is always there)
	std::vector<std::pair<int, std::string>> agc;          // code, name
	bool split = false;
	std::vector<std::string> parms_get, parms_set, vfo_ops;
	std::map<std::string, std::array<double, 3>> parm_range;      // numeric parameters: min, max, step
	std::map<std::string, std::vector<std::string>> parm_enum;   // parameters with named values (KEYERTYPE: STRAIGHT, BUG, PADDLE)
	std::vector<std::string> can_get, can_set;                    // "Can get RIT: Y" -> "RIT" (E counts: emulated)
	std::vector<int> ctcss;                                       // tones, tenths of Hz
	int rit_max = 0, xit_max = 0;                                 // Hz
	std::string model_name;                                       // "IC-7300"
	bool ic7300() const { return model_name == "IC-7300"; }
	bool can(const std::vector<std::string> &v, const std::string &k) const { for (const auto &x : v) if (x == k) return true; return false; }
};
// The radio's other settings, one rigctld get/set pair each (extended replies
// checked against rigctld -m 1, Hamlib 4.7.2, 2026-10-21). cap: the
// "Can get/set ..." line of \dump_caps; field: the reply's key.
struct RigSetting { const char *key, *cap, *get, *set, *field; bool num; };
const std::vector<RigSetting> &rig_settings();
// (The tables here are never freed: the rig thread can still be polling while exit() runs the
// static destructors, which freed them under it: ASAN, heap-use-after-free, 2026-10-21.)
// IC-7300 menu items Hamlib has no command for, read and set as raw CI-V
// through rigctld's send_cmd_rx ("W", Hamlib 4.7.2: the reply comes back
// after the radio's echo of the command; checked on radio 2, 2026-10-21).
// Menu numbers and values from the bench (the operating notes 3,
// winlink/radio2.py): 0067 DATA MOD 0 MIC 1 ACC 2 MIC+ACC 3 USB; 0078 USB
// SEND 0 OFF 1 DTR 2 RTS; 0065 USB MOD level 0..255 (two BCD bytes).
struct CivSetting { const char *key; uint8_t hi, lo; int bytes, max; };
inline const std::vector<CivSetting> &civ_settings()
{
	static const std::vector<CivSetting> &v = *new std::vector<CivSetting>{{"data_mod", 0x00, 0x67, 1, 3}, {"usb_send", 0x00, 0x78, 1, 2}, {"usb_mod_level", 0x00, 0x65, 2, 255}};
	return v;
}
// The radio's CI-V address for those (the IC-7300's default; Hamlib's own is not exposed by rigctld).
constexpr uint8_t kCivAddr7300 = 0x94;
// Functions that are actions rather than states: not polled (reading them means nothing).
inline bool rig_func_action(const std::string &k) { return k == "SEND_MORSE" || k == "SEND_VOICE_MEM" || k == "TBURST"; }
// Every level, function and parameter the radio reports is read every few
// polls; meters every poll while transmitting. Nothing is blocked, the
// transmitting ones included (PTT, TUNE, SEND_MORSE): wide open for
// development (user, 2026-10-21); a password for TX functions comes before
// shipping (the to-do list).
inline const std::vector<std::string> &rig_tx_meters()
{
	static const std::vector<std::string> &v = *new std::vector<std::string>{"RFPOWER_METER_WATTS", "RFPOWER_METER", "SWR", "ALC", "COMP_METER", "ID_METER"};
	return v;
}
inline bool rig_is_meter(const std::string &k) { return k.size() > 6 && k.compare(k.size() - 6, 6, "_METER") == 0; }
// Read-only measurements: STRENGTH and RAWSTR every poll (the S-meter), the TX meters while transmitting.
inline bool rig_is_measure(const std::string &k)
{
	if (k == "STRENGTH" || k == "RAWSTR") return true;
	for (const auto &x : rig_tx_meters()) if (x == k) return true;
	return false;
}

struct RigState {
	bool connected = false;
	std::map<std::string, double> levels;  // every settable level, and while transmitting the TX meters
	std::map<std::string, int> funcs;
	std::map<std::string, std::string> settings;   // rig_settings() keys, split_mode, split_passband, tx_vfo
	std::map<std::string, std::string> parms;      // the radio's parameters (BEEP, BACKLIGHT, ...)
	int split = -1;
	double watts_full = 0;             // the radio's maximum power here (Hamlib power2mW of RFPOWER 1), W; 0 unknown
	double freq_hz = 0;
	std::string mode;
	int passband = 0;
	int ptt = -1;                      // -1 unknown
	int smeter_db = -999;              // dB relative to S9 (Hamlib STRENGTH); -999 unknown
	std::string error;                 // the last problem, "" none
	long polls = 0;
	int own_pid = 0;
};

class RigClient {
public:
	~RigClient() { stop(); }
	// Starts (or restarts, when the connection changed) with cfg. Returns at once.
	void configure(const RigConfig &cfg)
	{
		std::lock_guard<std::mutex> g(m_);
		const bool relink = !running_ || !cfg.same_link(cfg_);
		const bool freq_changed = std::fabs(cfg.freq_khz - cfg_.freq_khz) > 1e-6, mode_changed = cfg.mode != cfg_.mode || cfg.passband != cfg_.passband;
		cfg_ = cfg;
		if (relink) { m_.unlock(); stop(); m_.lock(); cfg_ = cfg; }
		if (!cfg_.enabled()) return;
		if (freq_changed && cfg_.freq_khz > 0) lower_to_cap(cfg_.freq_khz * 1000.0);
		else if (running_) lower_to_cap(st_.freq_hz);          // a cap lowered here
		if (freq_changed && cfg_.freq_khz > 0) q_.push_back("+F " + std::to_string(long(std::llround(cfg_.freq_khz * 1000.0))));
		if (mode_changed && !cfg_.mode.empty()) q_.push_back("+M " + cfg_.mode + " " + std::to_string(cfg_.passband));
		if (relink) { stop_ = false; running_ = true; th_ = std::thread([this] { run(); }); }
	}
	void stop()
	{
		{ std::lock_guard<std::mutex> g(m_); if (!running_) return; stop_ = true; }
		if (th_.joinable()) th_.join();
		std::lock_guard<std::mutex> g(m_);
		running_ = false;
		q_.clear();
		kill_own();
		st_ = RigState{};
	}
	RigState state() const { std::lock_guard<std::mutex> g(m_); return st_; }
	RigConfig config() const { std::lock_guard<std::mutex> g(m_); return cfg_; }
	// Rises on every change of state() (for the caller to post updates).
	long version() const { return ver_.load(); }
	// One-off requests (the WebSocket's rig_set): queued for the thread.
	void set_freq_khz(double khz) { std::lock_guard<std::mutex> g(m_); cfg_.freq_khz = khz; lower_to_cap(khz * 1000.0); q_.push_back("+F " + std::to_string(long(std::llround(khz * 1000.0)))); refresh_ = true; }
	void set_mode(const std::string &mode, int pb) { std::lock_guard<std::mutex> g(m_); cfg_.mode = mode; cfg_.passband = pb; q_.push_back("+M " + mode + " " + std::to_string(pb)); }
	RigCaps caps() const { std::lock_guard<std::mutex> g(m_); return caps_; }
	// A level, function or split (validated by the caller against caps()); the settings are read back at once.
	// Hamlib turns a 0..1 level into the radio's steps by truncating (IC-7300: 0.952941 = 243/255 came back
	// as 242/255, 2026-10-21): half a step is added so the step nearest the value is set.
	void set_level(const std::string &k, double v)
	{
		std::lock_guard<std::mutex> g(m_);
		auto it = caps_.range.find(k);
		if (it != caps_.range.end() && it->second[1] <= 1.0 && it->second[2] > 0 && v > 0) v = std::min(it->second[1], v + it->second[2] * 0.5);
		char b[48]; std::snprintf(b, sizeof b, "%.6g", v);
		q_.push_back("+L " + k + " " + b); refresh_ = true;
	}
	// A function's value: 0 / 1, or more for some (IC-7300 break-in reads SBKIN 2 = full).
	void set_func(const std::string &k, int v) { std::lock_guard<std::mutex> g(m_); q_.push_back("+U " + k + " " + std::to_string(v)); refresh_ = true; }
	void set_split(bool on) { std::lock_guard<std::mutex> g(m_); q_.push_back(std::string("+S ") + (on ? "1" : "0") + " VFOB"); refresh_ = true; }
	// The rest (validated by the caller): one rig_settings() entry, the split
	// mode, a parameter, a VFO operation.
	void set_setting(const RigSetting &s, const std::string &v) { std::lock_guard<std::mutex> g(m_); q_.push_back(std::string(s.set) + " " + v); refresh_ = true; }
	void set_split_mode(const std::string &mode, int pb) { std::lock_guard<std::mutex> g(m_); q_.push_back("+X " + mode + " " + std::to_string(pb)); refresh_ = true; }
	void set_parm(const std::string &k, const std::string &v)
	{
		std::lock_guard<std::mutex> g(m_);
		std::string x = v;
		auto it = caps_.parm_range.find(k);                // a 0..1 parameter: half a step, as set_level
		if (it != caps_.parm_range.end() && it->second[1] <= 1.0 && it->second[2] > 0 && std::atof(v.c_str()) > 0) {
			char b[48]; std::snprintf(b, sizeof b, "%.6g", std::min(it->second[1], std::atof(v.c_str()) + it->second[2] * 0.5)); x = b;
		}
		q_.push_back("+P " + k + " " + x); refresh_ = true;
	}
	void vfo_op(const std::string &op) { std::lock_guard<std::mutex> g(m_); q_.push_back("+\\vfo_op " + op); refresh_ = true; }
	// An IC-7300 menu item (civ_settings(), value checked by the caller).
	void set_civ(const CivSetting &s, int v)
	{
		std::vector<uint8_t> body = {0x1A, 0x05, s.hi, s.lo};
		if (s.bytes == 2) { body.push_back(uint8_t(v / 100)); body.push_back(uint8_t(((v / 10 % 10) << 4) | v % 10)); }
		else body.push_back(uint8_t(v));
		std::string h = "#civ ";                     // made into a W command on the thread, which knows the radio's echo
		for (uint8_t b : body) { char t[4]; std::snprintf(t, sizeof t, "%02X", b); h += t; }
		std::lock_guard<std::mutex> g(m_); q_.push_back(h); refresh_ = true;   // the answer: FE FE E0 94 FB FD
	}
	// "+W \0xFE\0xFE\0x94\0xE0 <body> \0xFD N": N counts the radio's echo of the command when its
	// CI-V USB echo is on (an IC-7300 menu item; on the bench radio 2 has it on, radio 1 off).
	std::string civ_cmd(const std::vector<uint8_t> &body, int answer_bytes) const
	{
		std::string c = "+W ";
		auto hx = [&](uint8_t b) { char t[8]; std::snprintf(t, sizeof t, "\\0x%02X", b); c += t; };
		hx(0xFE); hx(0xFE); hx(kCivAddr7300); hx(0xE0);
		for (uint8_t b : body) hx(b);
		hx(0xFD);
		return c + " " + std::to_string((civ_echo_ == 1 ? int(body.size()) + 5 : 0) + answer_bytes);
	}
	// The bytes of a "Reply: \0xFE\0xFE..." line.
	static std::vector<uint8_t> civ_bytes(const std::vector<std::string> &L)
	{
		std::vector<uint8_t> v;
		for (const auto &l : L) {
			const size_t p = l.find("Reply:");
			if (p == std::string::npos) continue;
			for (size_t i = l.find("\\0x", p); i != std::string::npos; i = l.find("\\0x", i + 3)) v.push_back(uint8_t(std::strtoul(l.substr(i + 3, 2).c_str(), nullptr, 16)));
		}
		return v;
	}
	void set_ptt(bool on) { std::lock_guard<std::mutex> g(m_); if (on) lower_to_cap(st_.freq_hz); q_.push_back(on ? "+T 1" : "+T 0"); }
	void send_morse(const std::string &text) { std::lock_guard<std::mutex> g(m_); q_.push_back("+b " + text); }
	// While a link is up: frequency, PTT and the S-meter only (and the TX meters),
	// so our queries never sit in rigctld in front of a CAT key command.
	void set_busy(bool b) { busy_ = b; }
	std::string caps_json() const
	{
		const RigCaps c = caps();
		auto arr = [](const std::vector<std::string> &v) { std::string j = "["; for (const auto &x : v) j += (j.size() > 1 ? ",\"" : "\"") + clean(x) + "\""; return j + "]"; };
		std::string j = "{\"ok\":" + std::string(c.ok ? "true" : "false") + ",\"levels_get\":" + arr(c.levels_get) + ",\"levels_set\":" + arr(c.levels_set) +
		                ",\"funcs_get\":" + arr(c.funcs_get) + ",\"funcs_set\":" + arr(c.funcs_set) + ",\"modes\":" + arr(c.modes) + ",\"vfos\":" + arr(c.vfos) + ",\"range\":{";
		bool f = true;
		for (const auto &kv : c.range) { char b[128]; std::snprintf(b, sizeof b, "%s\"%s\":[%g,%g,%g]", f ? "" : ",", clean(kv.first).c_str(), kv.second[0], kv.second[1], kv.second[2]); j += b; f = false; }
		j += "},\"preamp\":[0"; for (int x : c.preamp) j += "," + std::to_string(x);
		j += "],\"att\":[0"; for (int x : c.att) j += "," + std::to_string(x);
		j += "],\"agc\":["; f = true;
		for (const auto &a : c.agc) { j += (f ? "" : ",") + std::string("[") + std::to_string(a.first) + ",\"" + clean(a.second) + "\"]"; f = false; }
		j += "],\"split\":" + std::string(c.split ? "true" : "false");
		j += ",\"parms_get\":" + arr(c.parms_get) + ",\"parms_set\":" + arr(c.parms_set) + ",\"vfo_ops\":" + arr(c.vfo_ops) + ",\"parm_range\":{";
		f = true;
		for (const auto &kv : c.parm_range) { char b[128]; std::snprintf(b, sizeof b, "%s\"%s\":[%g,%g,%g]", f ? "" : ",", clean(kv.first).c_str(), kv.second[0], kv.second[1], kv.second[2]); j += b; f = false; }
		j += "},\"parm_enum\":{"; f = true;
		for (const auto &kv : c.parm_enum) { j += (f ? "\"" : ",\"") + clean(kv.first) + "\":" + arr(kv.second); f = false; }
		j += "},\"settings_get\":["; f = true;
		for (const auto &s : rig_settings()) if (c.can(c.can_get, s.cap)) { j += (f ? "\"" : ",\"") + std::string(s.key) + "\""; f = false; }
		if (c.ic7300()) for (const auto &s : civ_settings()) { j += (f ? "\"" : ",\"") + std::string(s.key) + "\""; f = false; }
		j += "],\"settings_set\":["; f = true;
		for (const auto &s : rig_settings()) if (c.can(c.can_set, s.cap)) { j += (f ? "\"" : ",\"") + std::string(s.key) + "\""; f = false; }
		if (c.ic7300()) for (const auto &s : civ_settings()) { j += (f ? "\"" : ",\"") + std::string(s.key) + "\""; f = false; }
		j += "],\"split_mode\":" + std::string(c.can(c.can_set, "Split Mode") ? "true" : "false") + ",\"ctcss\":[";
		f = true; for (int t : c.ctcss) { j += (f ? "" : ",") + std::to_string(t); f = false; }
		return j + "],\"rit_max\":" + std::to_string(c.rit_max) + ",\"xit_max\":" + std::to_string(c.xit_max) +
		       ",\"morse\":" + (c.can(c.can_set, "send Morse") ? "true" : "false") + ",\"model_name\":\"" + clean(c.model_name) + "\"}";
	}
	std::string json() const
	{
		const RigState s = state(); const RigConfig c = config();
		char b[512];
		std::snprintf(b, sizeof b, "{\"control\":\"%s\",\"connected\":%s,\"freq_khz\":%.3f,\"mode\":\"%s\",\"passband\":%d,\"ptt\":%d,\"smeter_db\":%d,\"error\":\"%s\",\"polls\":%ld,"
		              "\"split\":%d,\"watts_full\":%.1f,\"max_rfpower\":%.4f,\"rfpower_cap\":%.4f,\"band\":\"%s\",\"busy\":%s",
		              c.control.c_str(), s.connected ? "true" : "false", s.freq_hz / 1000.0, clean(s.mode).c_str(), s.passband, s.ptt, s.smeter_db, clean(s.error).c_str(), s.polls,
		              s.split, s.watts_full, c.max_rfpower, c.cap_at(s.freq_hz), ham_band(s.freq_hz / 1000.0), busy_.load() ? "true" : "false");
		std::string j = b;
		j += ",\"levels\":{"; bool f = true;
		for (const auto &kv : s.levels) { char t[96]; std::snprintf(t, sizeof t, "%s\"%s\":%.6g", f ? "" : ",", clean(kv.first).c_str(), kv.second); j += t; f = false; }
		j += "},\"funcs\":{"; f = true;
		for (const auto &kv : s.funcs) { j += (f ? "\"" : ",\"") + clean(kv.first) + "\":" + std::to_string(kv.second); f = false; }
		auto val = [](const std::string &x) {      // a number as a number, anything else as a string
			char *e = nullptr; std::strtod(x.c_str(), &e);
			return !x.empty() && e && *e == 0 ? x : "\"" + clean(x) + "\"";
		};
		j += "},\"settings\":{"; f = true;
		for (const auto &kv : s.settings) { j += (f ? "\"" : ",\"") + clean(kv.first) + "\":" + val(kv.second); f = false; }
		j += "},\"parms\":{"; f = true;
		for (const auto &kv : s.parms) { j += (f ? "\"" : ",\"") + clean(kv.first) + "\":" + val(kv.second); f = false; }
		return j + "}}";
	}

private:
	mutable std::mutex m_;
	RigConfig cfg_;
	RigState st_;
	std::deque<std::string> q_;
	std::thread th_;
	bool running_ = false, stop_ = false;
	std::atomic<long> ver_{0};
	std::atomic<bool> busy_{false};
	RigCaps caps_;
	bool refresh_ = true;                // read the settings at the next poll
	int own_pid_ = 0;
	int civ_echo_ = -1;                  // the radio's CI-V echo: -1 not yet known (per connection)
	std::chrono::steady_clock::time_point answered_ = std::chrono::steady_clock::now();   // rigctld's last answer (own: the hang watchdog)
	std::string rbuf_;

	static std::string clean(std::string s) { for (auto &c : s) if (c == '"' || c == '\\' || uint8_t(c) < 32) c = ' '; return s; }
	void kill_own()
	{
		if (own_pid_ > 0) { ::kill(own_pid_, SIGTERM); for (int i = 0; i < 20 && ::waitpid(own_pid_, nullptr, WNOHANG) == 0; i++) ::usleep(50000); ::kill(own_pid_, SIGKILL); for (int i = 0; i < 40 && ::waitpid(own_pid_, nullptr, WNOHANG) == 0; i++) ::usleep(50000); own_pid_ = 0; }   // (reaped: no zombie)
	}
	// The RF power last read is above the cap at hz: queue it down to the cap first (m_ held).
	// Hamlib truncates a level to the radio's step, so the radio ends at or below the cap.
	void lower_to_cap(double hz)
	{
		auto it = st_.levels.find("RFPOWER");
		if (hz <= 0 || it == st_.levels.end() || !caps_.can(caps_.levels_set, "RFPOWER")) return;
		const double cap = cfg_.cap_at(hz);
		if (it->second <= cap + 1e-4) return;
		char b[48]; std::snprintf(b, sizeof b, "+L RFPOWER %.6g", cap);
		q_.push_back(b); it->second = cap; refresh_ = true;
	}
	// Our own rigctld still running but not answering (the IC-7300's USB serial stalled,
	// "cp210x ttyUSB0: ... urb stopped: -32", the bench Pi 2026-10-31: every client then
	// waited forever, Pat's QSY included): restarted, which reopens the serial port.
	void hung_own(const RigConfig &c)
	{
		if (c.control != "own" || own_pid_ <= 0) return;
		if (std::chrono::steady_clock::now() - answered_ < std::chrono::seconds(15)) return;
		std::fprintf(stderr, "[rig] own rigctld (pid %d) has not answered for 15 s: restarting it\n", own_pid_);
		kill_own();
		{ std::lock_guard<std::mutex> g(m_); st_.own_pid = 0; }
		if (!spawn_own(c)) set_error("own rigctld: could not restart rigctld");
		else set_error("own rigctld stopped answering: restarted");
	}
	void set_error(const std::string &e) { std::lock_guard<std::mutex> g(m_); if (st_.error != e) { st_.error = e; ver_++; } }
	bool spawn_own(const RigConfig &c)
	{
		std::vector<std::string> a = {"rigctld", "-m", std::to_string(c.model), "-T", "127.0.0.1", "-t", std::to_string(c.port)};
		if (!c.device.empty()) { a.push_back("-r"); a.push_back(c.device); }
		if (c.baud > 0) { a.push_back("-s"); a.push_back(std::to_string(c.baud)); }
		if (!c.conf.empty() && !c.device.empty()) a.push_back("--set-conf=" + c.conf);
		const pid_t pid = ::fork();
		if (pid < 0) return false;
		if (pid == 0) {
			::setpgid(0, 0);
			::prctl(PR_SET_PDEATHSIG, SIGTERM);          // dies with us (a crashed station leaves no rigctld holding the port)
			int dn = ::open("/dev/null", O_RDWR); if (dn >= 0) { ::dup2(dn, 0); ::dup2(dn, 1); ::dup2(dn, 2); }
			std::vector<char *> av; for (auto &s : a) av.push_back(const_cast<char *>(s.c_str())); av.push_back(nullptr);
			::execvp("rigctld", av.data());
			::_exit(127);
		}
		std::lock_guard<std::mutex> g(m_);
		own_pid_ = pid; st_.own_pid = pid;
		answered_ = std::chrono::steady_clock::now();
		return true;
	}
	// One extended command: the "Key: value" lines (or bare values) and the RPRT code.
	bool cmd(int fd, const std::string &c, std::vector<std::string> &lines, int &rprt)
	{
		lines.clear(); rprt = -99;
		const std::string w = c + "\n";
		if (::send(fd, w.data(), w.size(), MSG_NOSIGNAL) != ssize_t(w.size())) return false;
		const auto t0 = std::chrono::steady_clock::now();
		for (;;) {
			size_t p;
			while ((p = rbuf_.find('\n')) != std::string::npos) {
				std::string l = rbuf_.substr(0, p); rbuf_.erase(0, p + 1);
				if (!l.empty() && l.back() == '\r') l.pop_back();
				if (l.rfind("RPRT ", 0) == 0) { rprt = std::atoi(l.c_str() + 5); answered_ = std::chrono::steady_clock::now(); return true; }
				lines.push_back(l);
			}
			const int left = 3000 - int(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
			if (left <= 0) return false;
			pollfd pf{fd, POLLIN, 0};
			if (::poll(&pf, 1, left) != 1) return false;
			char b[1024];
			const ssize_t n = ::recv(fd, b, sizeof b, 0);
			if (n <= 0) return false;
			rbuf_.append(b, size_t(n));
		}
	}
	// "(0..20/10)" or "(0.000000..1.000000/0.003922)": sscanf's %lf would read "0." and lose the "..".
	static bool range3(const char *p, double &a, double &b, double &st)
	{
		if (*p != '(') return false;
		const char *dd = std::strstr(p, ".."), *sl = dd ? std::strchr(dd, '/') : nullptr;
		if (!dd || !sl) return false;
		a = std::atof(std::string(p + 1, dd).c_str()); b = std::atof(std::string(dd + 2, sl).c_str()); st = std::atof(sl + 1);
		return true;
	}
	static std::vector<std::string> words(const std::string &s)
	{
		std::vector<std::string> w; std::string cur;
		for (char ch : s) { if (ch == ' ' || ch == '\t') { if (!cur.empty()) w.push_back(cur); cur.clear(); } else cur += ch; }
		if (!cur.empty()) w.push_back(cur);
		return w;
	}
	// rigctld's \dump_caps text: "Get level: AF(0.000000..1.000000/0.003922) ...", "Preamp: 1dB 2dB", "AGC levels: 0=OFF 2=FAST".
	static RigCaps parse_caps(const std::vector<std::string> &L)
	{
		RigCaps c;
		auto after = [](const std::string &l, const char *k) -> const char * { const size_t n = std::strlen(k); return l.compare(0, n, k) == 0 ? l.c_str() + n : nullptr; };
		auto levels = [&](const char *rest, std::vector<std::string> &out) {
			for (const auto &w : words(rest)) {
				const size_t p = w.find('(');
				const std::string k = w.substr(0, p);
				out.push_back(k);
				double a = 0, b = 0, st = 0;
				if (p != std::string::npos && range3(w.c_str() + p, a, b, st)) c.range[k] = {a, b, st};
			}
		};
		for (const auto &l : L) {
			const char *r;
			if ((r = after(l, "Get level:"))) levels(r, c.levels_get);
			else if ((r = after(l, "Set level:"))) levels(r, c.levels_set);
			else if ((r = after(l, "Get functions:"))) c.funcs_get = words(r);
			else if ((r = after(l, "Set functions:"))) c.funcs_set = words(r);
			else if ((r = after(l, "Mode list:"))) c.modes = words(r);
			else if ((r = after(l, "VFO list:"))) c.vfos = words(r);
			else if ((r = after(l, "Preamp:"))) { for (const auto &w : words(r)) if (std::atoi(w.c_str()) > 0) c.preamp.push_back(std::atoi(w.c_str())); }
			else if ((r = after(l, "Attenuator:"))) { for (const auto &w : words(r)) if (std::atoi(w.c_str()) > 0) c.att.push_back(std::atoi(w.c_str())); }
			else if ((r = after(l, "AGC levels:"))) { for (const auto &w : words(r)) { const size_t e = w.find('='); if (e != std::string::npos) c.agc.push_back({std::atoi(w.c_str()), w.substr(e + 1)}); } }
			else if ((r = after(l, "Can set Split VFO:"))) { c.split = std::strchr(r, 'Y') != nullptr; c.can_set.push_back("Split VFO"); }
			else if ((r = after(l, "Get parameters:")) || (r = after(l, "Set parameters:"))) {
				const bool set = l[0] == 'S';
				for (const auto &w : words(r)) {
					const size_t p = w.find('(');
					const std::string k = w.substr(0, p);
					(set ? c.parms_set : c.parms_get).push_back(k);
					if (p == std::string::npos) continue;
					double a = 0, b = 0, st = 0;
					if (range3(w.c_str() + p, a, b, st)) c.parm_range[k] = {a, b, st};
					else {                                 // (STRAIGHT,BUG,PADDLE)
						std::vector<std::string> e; std::string cur;
						for (size_t i = p + 1; i < w.size(); i++) { if (w[i] == ',' || w[i] == ')') { if (!cur.empty()) e.push_back(cur); cur.clear(); } else cur += w[i]; }
						c.parm_enum[k] = e;
					}
				}
			}
			else if ((r = after(l, "VFO Ops:"))) c.vfo_ops = words(r);
			else if ((r = after(l, "Model name:"))) { while (*r == ' ' || *r == '\t') r++; c.model_name = r; while (!c.model_name.empty() && c.model_name.back() == ' ') c.model_name.pop_back(); }
			else if ((r = after(l, "CTCSS:"))) { for (const auto &w : words(r)) { if (w.find('.') == std::string::npos) break; c.ctcss.push_back(int(std::lround(std::atof(w.c_str()) * 10))); } }
			else if ((r = after(l, "Max RIT:")) || (r = after(l, "Max XIT:"))) {
				const char *sl = std::strchr(r, '/');         // "-9.999kHz/+9.999kHz"
				const int hz = sl ? int(std::lround(std::atof(sl + 1) * (std::strstr(sl, "kHz") ? 1000.0 : 1.0))) : 0;
				(l[4] == 'R' ? c.rit_max : c.xit_max) = hz;
			}
			else if ((r = after(l, "Can send Morse:"))) { if (std::strchr(r, 'Y')) c.can_set.push_back("send Morse"); }
			else if (l.compare(0, 8, "Can get ") == 0 || l.compare(0, 8, "Can set ") == 0) {
				const size_t e = l.find(':');
				if (e != std::string::npos && l.find_first_of("YE", e) != std::string::npos)
					(l[4] == 'g' ? c.can_get : c.can_set).push_back(l.substr(8, e - 8));
			}
		}
		c.ok = !c.levels_get.empty() || !c.funcs_get.empty();
		return c;
	}
	static std::string value(const std::vector<std::string> &lines, const char *key)
	{
		for (const auto &l : lines) {
			const size_t c = l.find(':');
			if (c != std::string::npos && l.compare(0, c, key) == 0) { size_t s = c + 1; while (s < l.size() && l[s] == ' ') s++; return l.substr(s); }
		}
		return "";
	}
	void run()
	{
		RigConfig c; { std::lock_guard<std::mutex> g(m_); c = cfg_; }
		if (c.control == "own") {
			if (c.model <= 0) { set_error("own rigctld: no model"); }
			else if (!spawn_own(c)) set_error("own rigctld: could not start rigctld");
		}
		int fd = -1;
		auto next_try = std::chrono::steady_clock::now();
		bool first = true;
		while (true) {
			{ std::lock_guard<std::mutex> g(m_); if (stop_) break; c = cfg_; }
			if (c.control == "own" && own_pid_ > 0 && ::waitpid(own_pid_, nullptr, WNOHANG) == own_pid_) {
				std::lock_guard<std::mutex> g(m_); own_pid_ = 0; st_.own_pid = 0; st_.error = "own rigctld exited (model, device, baud?)"; ver_++;
			}
			if (fd < 0) {
				if (std::chrono::steady_clock::now() < next_try) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); continue; }
				fd = tcp_connect(c.host, c.port, 1000);
				next_try = std::chrono::steady_clock::now() + std::chrono::seconds(2);
				if (fd < 0) { set_error("no rigctld at " + c.host + ":" + std::to_string(c.port)); { std::lock_guard<std::mutex> g(m_); if (st_.connected) { st_.connected = false; ver_++; } } hung_own(c); continue; }
				rbuf_.clear();
				civ_echo_ = -1;
				{ std::lock_guard<std::mutex> g(m_); st_.connected = true; st_.error.clear(); ver_++; }
				{                                          // what this radio can do
					std::vector<std::string> L; int rp;
					if (cmd(fd, "+\\dump_caps", L, rp) && rp == 0) { RigCaps cp = parse_caps(L); std::lock_guard<std::mutex> g(m_); caps_ = cp; refresh_ = true; ver_++; }
				}
				if (first) {                               // the profile's frequency and mode on (first) connect
					first = false;
					std::lock_guard<std::mutex> g(m_);
					if (c.freq_khz > 0) q_.push_front("+F " + std::to_string(long(std::llround(c.freq_khz * 1000.0))));
					if (!c.mode.empty()) q_.push_back("+M " + c.mode + " " + std::to_string(c.passband));
				}
			}
			std::vector<std::string> L; int rp;
			bool ok = true;
			bool ic7300; { std::lock_guard<std::mutex> g(m_); ic7300 = caps_.ic7300(); }
			if (civ_echo_ < 0 && ic7300) {                 // TX state (1C 00) asked with an answer-only length: an echo shows as our own frame
				if ((ok = cmd(fd, "+W \\0xFE\\0xFE\\0x94\\0xE0\\0x1C\\0x00\\0xFD 8", L, rp)) && rp == 0) {
					const std::vector<uint8_t> b = civ_bytes(L);
					civ_echo_ = b.size() >= 4 && b[0] == 0xFE && b[1] == 0xFE && b[2] == kCivAddr7300 && b[3] == 0xE0;
					if (civ_echo_) { std::this_thread::sleep_for(std::chrono::milliseconds(300)); ok = cmd(fd, "+t", L, rp); }   // the rest of that answer drains
				}
			}
			for (;;) {                                     // queued sets first
				std::string q;
				{ std::lock_guard<std::mutex> g(m_); if (q_.empty()) break; q = q_.front(); q_.pop_front(); }
				if (q.rfind("#civ ", 0) == 0) {
					if (civ_echo_ < 0) continue;            // not probed (not an IC-7300): dropped
					std::vector<uint8_t> body;
					for (size_t i = 5; i + 1 < q.size(); i += 2) body.push_back(uint8_t(std::strtoul(q.substr(i, 2).c_str(), nullptr, 16)));
					q = civ_cmd(body, 6);
				}
				if (!(ok = cmd(fd, q, L, rp))) break;
				if (std::getenv("RIG_DEBUG")) std::fprintf(stderr, "[rig] %s -> RPRT %d\n", q.c_str(), rp);
				if (rp != 0) set_error(q.substr(1) + ": rigctld error " + std::to_string(rp));
			}
			RigState n; { std::lock_guard<std::mutex> g(m_); n = st_; }
			const double prev_hz = n.freq_hz;
			if (ok && (ok = cmd(fd, "+f", L, rp)) && rp == 0) n.freq_hz = std::atof(value(L, "Frequency").c_str());
			if (ok && (ok = cmd(fd, "+m", L, rp)) && rp == 0) { n.mode = value(L, "Mode"); n.passband = std::atoi(value(L, "Passband").c_str()); }
			if (ok && (ok = cmd(fd, "+t", L, rp)) && rp == 0) n.ptt = std::atoi(value(L, "PTT").c_str());
			const bool new_band = std::string(ham_band(n.freq_hz / 1000.0)) != ham_band(prev_hz / 1000.0);
			if (ok && (ok = cmd(fd, "+l STRENGTH", L, rp))) n.smeter_db = rp == 0 && L.size() >= 2 ? std::atoi(L[1].c_str()) : -999;
			RigCaps cp; bool slow; { std::lock_guard<std::mutex> g(m_); cp = caps_; slow = refresh_ || (n.polls % 5 == 0); if (!busy_) refresh_ = false; }
			if (busy_) slow = false;
			auto level = [&](const std::string &k) {      // one level into n.levels (dropped if the radio refuses it)
				if (!ok) return;
				if ((ok = cmd(fd, "+l " + k, L, rp)) && rp == 0 && L.size() >= 2) n.levels[k] = std::atof(L[1].c_str()); else n.levels.erase(k);
			};
			// a new band (the dial turned at the radio): its cap is checked now, not at the next slow poll
			if (ok && new_band && !slow && cp.can(cp.levels_get, "RFPOWER")) level("RFPOWER");
			if (ok && n.ptt == 1) for (const auto &k : rig_tx_meters()) if (cp.can(cp.levels_get, k)) level(k);
			if (ok && n.ptt == 0) for (const auto &k : rig_tx_meters()) n.levels.erase(k);
			if (ok && slow) {
				for (const auto &k : cp.levels_get) if (!rig_is_measure(k)) level(k);
				for (const auto &k : cp.funcs_get) if (ok && !rig_func_action(k)) {
					if ((ok = cmd(fd, "+u " + k, L, rp)) && rp == 0 && L.size() >= 2) n.funcs[k] = std::atoi(L[1].c_str()); else n.funcs.erase(k);
				}
				if (ok && (ok = cmd(fd, "+s", L, rp)) && rp == 0) { n.split = std::atoi(value(L, "Split").c_str()); n.settings["tx_vfo"] = value(L, "TX VFO"); }
				for (const auto &s : rig_settings()) if (ok && cp.can(cp.can_get, s.cap)) {
					if ((ok = cmd(fd, s.get, L, rp)) && rp == 0) n.settings[s.key] = value(L, s.field); else n.settings.erase(s.key);
				}
				if (ok && cp.can(cp.can_get, "Split Mode")) {
					if ((ok = cmd(fd, "+x", L, rp)) && rp == 0) { n.settings["split_mode"] = value(L, "TX Mode"); n.settings["split_passband"] = value(L, "TX Passband"); }
				}
				// "get_parm: BEEP", "1". Not the named-value ones: Hamlib 4.7.2's rigctld dumps core on
				// get_parm KEYERTYPE (IC-7300, tardis 2026-10-21), which takes the connection with it.
				for (const auto &s : civ_settings()) if (ok && cp.ic7300() && civ_echo_ >= 0) {   // the radio's answer after the echo: FE FE E0 94 1A 05 hi lo value.. FD
					if ((ok = cmd(fd, civ_cmd({0x1A, 0x05, s.hi, s.lo}, 9 + s.bytes), L, rp)) && rp == 0) {
						const std::vector<uint8_t> b = civ_bytes(L);
						int v = -1;
						for (size_t i = 0; i + 8 + size_t(s.bytes) < b.size(); i++)
							if (b[i] == 0xFE && b[i + 1] == 0xFE && b[i + 2] == 0xE0 && b[i + 4] == 0x1A && b[i + 5] == 0x05 && b[i + 6] == s.hi && b[i + 7] == s.lo) {
								v = s.bytes == 2 ? b[i + 8] * 100 + (b[i + 9] >> 4) * 10 + (b[i + 9] & 15) : b[i + 8];
								break;
							}
						if (v >= 0) n.settings[s.key] = std::to_string(v); else n.settings.erase(s.key);
					} else n.settings.erase(s.key);
				}
				for (const auto &k : cp.parms_get) if (ok && !cp.parm_enum.count(k)) {
					if ((ok = cmd(fd, "+p " + k, L, rp)) && rp == 0 && L.size() >= 2) n.parms[k] = L[1]; else n.parms.erase(k);
				}
				// the radio's full power here (Hamlib power2mW), for watts in the GUI
				if (ok && n.freq_hz > 0 && !n.mode.empty() && (ok = cmd(fd, "+\\power2mW 1 " + std::to_string(long(n.freq_hz)) + " " + n.mode, L, rp)) && rp == 0)
					n.watts_full = std::atof(value(L, "Power mW").c_str()) / 1000.0;
			}
			// Above the cap at this frequency (set at the radio, or the dial moved to a band with a lower
			// one): reported, and lowered to the cap when the radio lets us set it.
			if (ok && cp.can(cp.levels_get, "RFPOWER") && n.levels.count("RFPOWER")) {
				const double cap = c.cap_at(n.freq_hz), pw = n.levels["RFPOWER"];
				const char *band = ham_band(n.freq_hz / 1000.0);
				const bool banded = c.band_rfpower.count(band) && c.band_rfpower.at(band) < c.max_rfpower;
				if (cap < 1.0 && pw > cap + 1e-4) {
					const bool lower = cp.can(cp.levels_set, "RFPOWER");
					char e[200]; std::snprintf(e, sizeof e, "RF power %.3f is above the station's cap %.3f%s%s%s", pw, cap, banded ? " on " : "", banded ? band : "", lower ? ": lowered" : "");
					n.error = e;
					if (lower) { char b[48]; std::snprintf(b, sizeof b, "+L RFPOWER %.6g", cap); std::lock_guard<std::mutex> g(m_); q_.push_back(b); refresh_ = true; }
				} else if (n.error.rfind("RF power ", 0) == 0) n.error.clear();
			}
			if (!ok) {
				::close(fd); fd = -1;
				{ std::lock_guard<std::mutex> g(m_); st_.connected = false; st_.error = "rigctld connection lost"; ver_++; }
				hung_own(c);
				continue;
			}
			{
				std::lock_guard<std::mutex> g(m_);
				n.polls = st_.polls + 1; n.connected = true; n.own_pid = st_.own_pid;
				if (n.error.rfind("RF power ", 0) != 0) n.error = st_.error.rfind("RF power ", 0) == 0 ? std::string() : st_.error;
				const bool changed = n.freq_hz != st_.freq_hz || n.mode != st_.mode || n.passband != st_.passband || n.ptt != st_.ptt || n.smeter_db != st_.smeter_db ||
				                     n.levels != st_.levels || n.funcs != st_.funcs || n.split != st_.split || n.watts_full != st_.watts_full || n.error != st_.error ||
				                     n.settings != st_.settings || n.parms != st_.parms;
				st_ = n;
				if (changed) ver_++;
			}
			for (int w = 0; w < std::max(1, c.poll_ms / 50); w++) {   // sleep, waking early for a queued set or stop
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
				std::lock_guard<std::mutex> g(m_); if (stop_ || !q_.empty()) break;
			}
		}
		if (fd >= 0) ::close(fd);
	}
};

}  // namespace radio

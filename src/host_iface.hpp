// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// host_iface.hpp: the reference unit serial interface (terminal mode and the
// WA8DED host modes) in front of the modem. One HostIface per client: the pty,
// and every stream client a front end attaches (src/tnc_core.hpp).
//
// Texts and defaults checked against the emulated reference firmware
// V.4.1r answer by answer; the
// parameters the firmware has and the modem lacks are kept and reported
// (src/station.hpp).
//
// Terminal mode (firmware-checked):
//   - no echo; a line ends in CR; the answer is "\r\n" + lines + "\r\ncmd: "
//     (an empty line: "\r\ncmd: "); unknown verbs "*** ERROR: PSE TYPE HELP",
//     out-of-range values "\a*** BAD ARGUMENT";
//   - verbs may be abbreviated down to their capital letters (MYcall: MY);
//   - RS (0x1E) asks for the status byte: the answer is 0x1E + the byte
//     (STatus 2: sent by itself on every change);
//   - while a link is set up or up, the terminal is in converse mode: typed
//     bytes go to the link, ESCchr (27) gives one command line ("\r\ncmd: ",
//     its answer, no new prompt), QRTChr (Ctrl-D) = orderly QRT, BKchr (Ctrl-Y)
//     = break-in while receiving, CHOchr (Ctrl-Y) = changeover while sending
//     (every mode, as the reference unit: text typed while receiving waits for
//     the sender's changeover or this key, and the sender hands over only on
//     this key; PDuplex 1 does both by itself);
//   - link texts: "*** NOW CALLING X", "*** CONNECTED TO X", "*** MYCALL
//     RECEIVED - AT  (200 Bd) - hh:mm:ss / DAY, DD-MON-YY", "*** DISCONNECTED
//     [/ QRT RECEIVED] AT - ...", "*** NO RESPONSE FROM X / hh:mm:ss / ...",
//     "*** TIMEOUT - DISCONNECTED AT - ...", then the stand-by text.
//
// CRC host mode (JHOST4), host -> TNC: AA AA ch ctl len-1 payload crc_lo crc_hi
//   (ctl bit 0: command; bit 7: the toggle; CRC-16/X.25 over ch..payload; AA
//   after the header stuffed as AA 00). TNC -> host: AA AA ch code payload crc.
//   Codes: 0 ok, 1 text + NUL, 2 error text + NUL, 3 link status + NUL,
//   7 data (len-1, data). Bad CRC: AA AA AA 55 (resend, please). A frame
//   repeating the last one's toggle AND content is answered with the last
//   answer again (fix 93: Pat's driver reuses toggles; it must always be
//   answered, or it stalls). JHOST1: the same without CRC and stuffing.
// Host commands: L, G (255: channels with output; 254: the status block),
//   C, D, DD, I, %T, @B, %O (changeover / break-in), %I (break-in), JHOST0,
//   every terminal parameter command, and '#' + any terminal command (answered 0, no text).
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "alc_warn.hpp"
#include "peer_id.hpp"
#include "station.hpp"
#include "tune_tone.hpp"

namespace signum {

inline uint16_t host_crc(const uint8_t *p, size_t n)
{
	uint16_t c = 0xffff;
	for (size_t i = 0; i < n; i++) {
		c ^= p[i];
		for (int k = 0; k < 8; k++) c = (c & 1) ? uint16_t((c >> 1) ^ 0x8408) : uint16_t(c >> 1);
	}
	return uint16_t(c ^ 0xffff);
}

// Shared by every client: the station and what is common to all (the last
// call, Unproto mode, the version text).
struct HostShared {
	Station *stn = nullptr;
	std::string version_text;           // Version's answer (the program fills it)
	std::string last_call, calling;     // the last call placed (C without a call), the call being placed
	int up_mode = 1, up_reps = 2;       // Unproto: the firmware's default 1*2
	std::string ctext = "\rSignum: P1/P2/P3 station\r";   // the connect text (host mode U; the reference unit's own default is its own greeting)
	bool had_link = false;
	int last_level = 0;                 // the level of the current/last link (MYLevel)
	int cycle = 2;                      // CYcle (kept)
	std::string cwid = "0  2";          // CWid (kept), as the firmware shows it after CWID 0
	int mark = 1400, space = 1200, tones = 0;   // TOnes 0 = 1200/1400 (the firmware's default)
	std::function<std::string()> cpu_status;    // CPULoad's answer (the core's CPU health, src/cpu_health.hpp)
	std::function<std::string(const std::string &)> icpower;   // ICPower's answer (the IC-7300's RF POWER, src/radio/ic7300_power.hpp)
	TuneTone tune;                      // the tuning tone before a new call (TUnetone; off by default)
	AlcWarn alc;                        // the ALC warning (src/alc_warn.hpp; ALCwarn, --alc-warn)
	PeerId peer;                        // is the other station a Signum (src/peer_id.hpp; the marker: PEERmark)
	// The encryption layer, when built in (src/crypto/; empty: data goes straight to the modem):
	std::function<void(const std::string &)> link_send;   // host data for the link
	std::function<bool()> take_qrt;                        // true: the layer QRTs itself after its held data
	std::function<size_t()> held;                          // bytes it holds for the link
	std::function<std::string()> unproto_refused;          // non-empty: why Unproto is refused (the answer)
	std::function<std::string(const std::string &)> crypto_cmd;   // the CRYPTO command
	// A new outgoing call: with the tuning on (secs > 0), the radio's tune or the tone first (src/tune_tone.hpp), the call after it.
	void place_call(const std::string &c)
	{
		if (tune.secs > 0 && stn->quiet() && !tune.active()) tune.start(c, stn->config().amplitude, stn->config().fs);
		else stn->st().connect(c);
	}
};

class HostIface {
public:
	using LinkEvent = fmodem::P2Station::LinkEvent;
	explicit HostIface(HostShared &sh) : sh_(sh) {}

	// Bytes from the client; bytes for it accumulate (take_out).
	void input(const uint8_t *p, size_t n)
	{
		for (size_t i = 0; i < n; i++) {
			if (mode_ == TERMINAL) term_byte(p[i]);
			else host_byte(p[i]);
		}
	}
	// Received link text and link events (every client gets them), then the status byte.
	void deliver(const std::string &rx, const std::deque<LinkEvent> &ev);
	// A notice line (listen mode), terminal mode only.
	void notice(const std::string &line) { if (mode_ == TERMINAL) out_ += "\r\n" + line + "\r\n"; }
	std::string take_out() { std::string s; s.swap(out_); return s; }
	std::string take_log() { std::string s; s.swap(log_); return s; }
	bool host_mode() const { return mode_ != TERMINAL; }
	int cmsg() const { return cmsg_; }   // CMsg (the connect text on/off)
	bool listen() const { return stn().quiet() && stn().params().listen != 0; }
	// One command line from elsewhere (a front end): the terminal's answer, no prompt.
	std::string command(const std::string &l);

private:
	enum Mode { TERMINAL, HOST_PLAIN, HOST_CRC };
	struct Item { int code; std::string text; };
	Station &stn() const { return *sh_.stn; }
	fmodem::P2Station &st() const { return sh_.stn->st(); }
	HostParams &par() const { return sh_.stn->params(); }
	int hostch() const { return par().hostch; }

	// ---- terminal mode ------------------------------------------------------
	bool converse() const { return st().unproto() || st().link_up() || st().setting_up() || sh_.tune.active(); }
	void term_byte(uint8_t b);
	void term_line(const std::string &l);
	// A number argument in [lo, hi] (else false: BAD ARGUMENT).
	static bool num(const std::string &a, int lo, int hi, int &v)
	{
		if (a.empty()) return true;
		char *e = nullptr;
		const long x = std::strtol(a.c_str(), &e, 10);
		if (e == a.c_str() || x < lo || x > hi) return false;
		v = int(x);
		return true;
	}
	static std::string bad() { return "\a*** BAD ARGUMENT\r\n"; }
	// Parameter and information commands (terminal: the text; host mode: as code 1 text).
	// "" = not ours (ERROR).
	std::string general(const std::string &verb, const std::string &arg);
	std::string chr_param(int &v, const char *label, const std::string &arg)
	{
		int x = v;
		if (!num(arg, 1, 127, x) || x == 8 || x == 13 || x == 32 || x == 30 || x == 17 || x == 19) return bad();
		v = x;
		return std::string(label) + std::to_string(v) + "\r\n";
	}
	static std::string help();
	std::string banner() const
	{
		return "\r\nSignum P1/P2/P3 HF data modem\r\n" + sh_.version_text + stby_text();
	}
	// The stand-by text after D/DD/QRT at the prompt and after a link (the firmware's,
	// without its evaluation-copy line; 0x11 = XON, as the firmware sends it).
	static std::string stby_text() { return "\x11\r\n*** STBY >> \r\n"; }
	// "hh:mm:ss" or "DAY, DD-MON-YY" (host local time), as the firmware prints.
	static std::string clock(bool date);
	// The status byte (manual 6.92): bit 7 set; bits 6-4 mode (0 STBY, 2 ARQ,
	// 4 FEC (Unproto), 6 LISTEN); bit 3 we send; bits 2-0 0 ERROR, 1 REQUEST,
	// 2 TRAFFIC, 3 IDLE, 4 OVER, 6 SYNCH, 7 IGNORE. Idle with Listen on: 0xE7 (firmware).
	uint8_t status_byte() const;
	// Link events and received text, in the firmware's terminal format.
	void term_poll(const std::string &rx, const std::deque<LinkEvent> &ev)
	{
		// As host mode: the link-up text, the text received in this pass, then the end of the link.
		for (const auto &e : ev) if (e.up) term_event(e);
		for (char c : rx) { if (c == '\r') out_ += "\r\n"; else out_ += c; }
		for (const auto &e : ev) if (!e.up) term_event(e);
	}
	void term_event(const LinkEvent &e);

	// ---- host mode: framing ---------------------------------------------------
	void host_byte(uint8_t b);
	// Plain WA8DED (JHOST1): ch ctl len-1 payload, no CRC, no stuffing.
	void plain_byte(uint8_t b)
	{
		frame_.push_back(b);
		if (frame_.size() >= 3 && frame_.size() == size_t(frame_[2]) + 1 + 3) {
			handle(frame_[0], (frame_[1] & 1) != 0, std::string(frame_.begin() + 3, frame_.end()));
			frame_.clear();
		}
	}
	void reply(uint8_t ch, uint8_t code, const std::string &payload);
	void resend() { out_ += last_reply_; }
	static std::string strip_crlf(std::string t)
	{
		while (!t.empty() && (t.back() == '\n' || t.back() == '\r')) t.pop_back();
		while (!t.empty() && (t[0] == '\n' || t[0] == '\r' || t[0] == '\a')) t.erase(0, 1);
		return t;
	}

	// ---- host mode: commands ----------------------------------------------------
	void handle(uint8_t ch, bool cmd, const std::string &p);

	// '#' + a terminal command line (the reference unit's escape for terminal commands in host mode, as
	// LinBPQ sends #DD and #MYL n; checked on its emulator): carried out, and
	// answered code 0 with no text, whatever it was (its answer text is not sent, a query shows nothing, an
	// unknown command is ignored). D/DD/QRT end a link or an Unproto transmission, Connect calls, U starts
	// Unproto (its data then comes as data frames); JHOST is ignored here.
	void hash_cmd(uint8_t ch, std::string l);
	static bool is_call(const std::string &c)
	{
		if (c.size() < 3 || c.size() > 9) return false;
		bool digit = false;
		for (char x : c) { if (std::isdigit(uint8_t(x))) digit = true; else if (!std::isalpha(uint8_t(x)) && x != '-') return false; }
		return digit;
	}
	static std::string upper_str(std::string s) { for (auto &c : s) c = char(std::toupper(uint8_t(c))); return s; }
	static void split(const std::string &l, std::string &verb, std::string &arg);
	// terminal commands may be abbreviated: VERB must be a prefix of FULL, at least MIN letters.
	static bool abbrev(const std::string &verb, const char *full, size_t min)
	{
		const std::string f = full;
		return verb.size() >= min && verb.size() <= f.size() && f.compare(0, verb.size(), verb) == 0;
	}
	void log(const std::string &s) { log_ += "[host] " + s + "\n"; }
	// Link data and QRT through the encryption layer when there is one (HostShared).
	void link_send(const std::string &t) { if (sh_.link_send) sh_.link_send(t); else st().send_text(t); }
	void link_qrt() { if (!(sh_.take_qrt && sh_.take_qrt())) st().qrt(); }
	size_t held() const { return sh_.held ? sh_.held() : 0; }

	HostShared &sh_;
	Mode mode_ = TERMINAL;
	std::string line_, out_, log_, last_reply_, last_payload_;
	std::vector<uint8_t> frame_;
	int hdr_ = 0;
	bool stuffed_ = false, have_last_ = false;
	uint8_t last_ctl_ = 0, last_ch_ = 0, last_stb_ = 0;
	bool was_unproto_ = false;
	int up_end_ = 0;                                   // how this Unproto transmission is being ended: 1 QRT character, 2 D/QRT, 3 DD, 0 not by us
	bool esc_cmd_ = false, local_qrt_ = false, status_auto_ = false, linked_ = false, qrt_cmd_ = false;
	int cmsg_ = 1, kbox_ = 0, karx_ = 1, klf_ = 1, kbc_ = 1, kaddlf_ = 1;   // CMsg and the kept AMTOR/box parameters (firmware defaults)
	std::deque<Item> q_;
	std::string last_status_, stb_insert_;
};

}  // namespace signum

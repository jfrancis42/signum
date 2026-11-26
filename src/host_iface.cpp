// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// HostIface member functions (out of host_iface.hpp, so that they build in parallel).
#include "host_iface.hpp"

namespace signum {

void HostIface::deliver(const std::string &rx, const std::deque<LinkEvent> &ev)
{
	const bool u = st().unproto();                   // the end of an Unproto transmission (no link event: the reference unit's own text)
	if (was_unproto_ && !u && mode_ == TERMINAL) {
		if (up_end_ == 3) out_ += stby_text() + "\r\ncmd: ";                      // DD: the stand-by text only
		else out_ += "\r\n*** DISCONNECTED AT - " + clock(false) + " / " + clock(true) + "\r\n\r\n" + stby_text() + "\r\ncmd: ";
		esc_cmd_ = false; qrt_cmd_ = false;
	}
	if (!u) up_end_ = 0;
	was_unproto_ = u;
	if (mode_ == TERMINAL) {
		term_poll(rx, ev);
		if (status_auto_) { const uint8_t b = status_byte(); if (b != last_stb_) { last_stb_ = b; out_ += '\x1e'; out_ += char(b); } }
		return;
	}
	// CONNECTED first, then the text received in this pass (the caller's first text from its set-up packet
	// arrives with the link-up event), then DISCONNECTED: a host program drops data that comes before the
	// link it belongs to (the reference unit sends its CONNECTED before the link's first data).
	// HOST_ORDER_OLD=1: the data first, then every link message, as before (test).
	static const bool old = std::getenv("HOST_ORDER_OLD") != nullptr;
	for (const auto &e : ev) if (e.up && !old) q_.push_back({3, "(" + std::to_string(hostch()) + ") CONNECTED to " + e.call});
	for (size_t i = 0; i < rx.size(); i += 256) q_.push_back({7, rx.substr(i, 256)});
	for (const auto &e : ev) {
		if (e.up && old) q_.push_back({3, "(" + std::to_string(hostch()) + ") CONNECTED to " + e.call});
		if (!e.up) q_.push_back({3, "(" + std::to_string(hostch()) + ") DISCONNECTED fm " + e.call});
	}
}

std::string HostIface::command(const std::string &l)
{
	std::string verb, arg;
	split(l, verb, arg);
	if (verb == "JHOST4" || verb == "JHOST1" || verb == "JHOST0") return "*** ERROR: HOST MODE BELONGS TO A STREAM CLIENT (THE PTY OR /shell)\r\n";
	std::string keep;
	keep.swap(out_);
	term_line(l);
	qrt_cmd_ = false;
	std::string r;
	r.swap(out_);
	out_.swap(keep);
	return r;
}

void HostIface::term_byte(uint8_t b)
{
	if (b == 0x1e) { out_ += '\x1e'; out_ += char(status_byte()); return; }   // status request (any time)
	if (converse() && !esc_cmd_) {                   // a link: typed bytes are data
		if (b == par().escchr) { esc_cmd_ = true; line_.clear(); out_ += "\r\ncmd: "; }
		else if (b == par().qrtchr) { if (sh_.tune.cancel()) local_qrt_ = true; else { link_qrt(); if (st().unproto()) up_end_ = 1; else local_qrt_ = true; } log("QRT character: QRT"); }
		else if (b == par().bkchr || b == par().chochr) {   // break-in as receiver / changeover as sender
			const int k = (b == par().bkchr ? fmodem::P1Station::KEY_BK : 0) | (b == par().chochr ? fmodem::P1Station::KEY_CHO : 0);
			if (!st().changeover_key(k)) log("break-in/changeover character: nothing to do now");
		}
		else link_send(std::string(1, char(b)));
		return;
	}
	if (b == '\r') {
		const std::string l = line_;
		line_.clear();
		const bool was_esc = esc_cmd_;
		esc_cmd_ = false;
		std::string verb, arg;
		split(l, verb, arg);
		if (verb.empty()) { if (!converse() || was_esc) out_ += "\r\ncmd: "; return; }
		const size_t at = out_.size();
		out_ += "\r\n";
		term_line(l);
		if (!stb_insert_.empty()) { out_.insert(at, stb_insert_); stb_insert_.clear(); }   // (STatus 2: the byte comes first)
		// (after D/DD in a link the link-down text brings the prompt)
		if (mode_ == TERMINAL && !converse() && !qrt_cmd_) out_ += "\r\ncmd: ";
		qrt_cmd_ = false;
	} else if (b == par().escchr) { line_.clear(); esc_cmd_ = false; if (!converse()) out_ += "\r\ncmd: "; }
	else if (b == 0x08 || b == 0x7f) { if (!line_.empty()) line_.pop_back(); }
	else if (b >= 0x20 && b < 0x7f && line_.size() < 256) line_ += char(b);
	else if (b != '\n') line_.clear();                 // binary noise (a stray host frame): drop the line
}

void HostIface::term_line(const std::string &l)
{
	std::string verb, arg;
	split(l, verb, arg);
	log("term: " + l);
	if (verb.empty()) return;
	if (verb == "JHOST4" || verb == "JHOST1") {
		mode_ = verb == "JHOST4" ? HOST_CRC : HOST_PLAIN;
		hdr_ = 0; frame_.clear(); have_last_ = false; q_.clear();
		{ uint8_t stb[4] = {0, 0, 1, 128}; st().host_status(stb); if (stn().quiet() || st().unproto()) stb[0] = status_byte(); last_status_.assign(reinterpret_cast<char *>(stb), 4); }   // (ch 255 lists 254 only on a change)
		out_.clear();                                  // the firmware sends nothing after JHOST (capture: no answer)
		log(std::string("enter host mode ") + (mode_ == HOST_CRC ? "(CRC)" : "(plain)"));
		return;
	}
	if (abbrev(verb, "CONNECT", 1)) {
		std::string c = arg.empty() ? sh_.last_call : arg.substr(0, arg.find(' '));
		for (auto &x : c) x = char(std::toupper(uint8_t(x)));
		if (c.empty() || converse()) { out_ += "*** ERROR: PSE TYPE HELP\r\n"; return; }
		sh_.last_call = c;
		const bool lp = c.size() > 1 && (c[0] == '!' || c[0] == ';');   // long path (manual 6.22.1)
		out_ += "*** NOW CALLING " + (lp ? c.substr(1) + "  (LONGPATH-OPTION)" : c) + "\r\n";
		sh_.calling = lp ? c.substr(1) : c;
		sh_.place_call(c);
		return;
	}
	if (abbrev(verb, "DISCONNECT", 1) || verb == "DD" || abbrev(verb, "QRT", 1)) {
		if (st().unproto()) {                         // Unproto (reference unit): D / QRT say so at once, DD says nothing; the end follows in deliver()
			if (verb == "DD") { st().abort_link(); up_end_ = 3; }
			else { st().qrt(); up_end_ = 2; out_ += "*** QRT IN PROGRESS - TEXTINPUT TERMINATED\r\n"; }
			qrt_cmd_ = true;
			return;
		}
		if (converse()) { if (!sh_.tune.cancel()) { if (verb == "DD") st().abort_link(); else link_qrt(); } local_qrt_ = true; qrt_cmd_ = true; return; }
		out_ += stby_text();                          // as the firmware: the stand-by text
		return;
	}
	if (abbrev(verb, "RESTART", 4)) { if (converse()) { sh_.tune.cancel(); st().abort_link(); } out_ += banner(); return; }
	const std::string r = general(verb, arg);
	if (r.empty() && (verb == "U" || abbrev(verb, "UNPROTO", 1)) && converse()) {   // U with a link or Unproto up: nothing, not even the line break
		if (out_.size() >= 2 && out_.compare(out_.size() - 2, 2, "\r\n") == 0) out_.resize(out_.size() - 2);
		return;
	}
	out_ += r.empty() ? "*** ERROR: PSE TYPE HELP\r\n" : r;
}

std::string HostIface::general(const std::string &verb, const std::string &arg)
{
	HostParams &p = par();
	if (verb == "CRYPTO" && sh_.crypto_cmd) return sh_.crypto_cmd(arg);   // Signum's own (docs/md/commands.md); only the full word
	auto param = [&](int &val, int lo, int hi, const std::string &label, bool rebuild = false) -> std::string {
		int v = val;
		if (!num(arg, lo, hi, v)) return bad();
		if (v != val) { val = v; if (rebuild) stn().params_changed(); }
		return label + std::to_string(val) + "\r\n";
	};
	if (abbrev(verb, "MYCALL", 2)) {
		if (!arg.empty()) { std::string c = arg.substr(0, arg.find(' ')); for (auto &ch : c) ch = char(std::toupper(uint8_t(ch))); if (c.size() > 9) return bad(); st().set_mycall(c); }
		return "Mycall: >" + st().mycall() + "<\r\n";
	}
	if (abbrev(verb, "MYLEVEL", 3)) {
		int l = st().p1_only() ? 1 : st().allow_p3() ? 3 : 2;
		if (!num(arg, 1, 3, l)) return bad();
		if (!arg.empty()) { st().set_allow_p3(l >= 3); st().set_p1_only(l == 1); }
		return "*** HIGHEST POSSIBLE PACTOR LINK LEVEL: " + std::to_string(l) + "\r\n*** LINK LEVEL OF ACTUAL/LAST LINK: " +
		       std::to_string(sh_.last_level) + "\r\n";
	}
	if (abbrev(verb, "TONES", 2)) {
		int t = sh_.tones;
		if (!num(arg, 0, 4, t) || t == 3) return bad();   // 0 low, 1 high, 2 MARk/SPAce, 4 1400/1600 (the firmware takes 4)
		if (!arg.empty()) {
			sh_.tones = t;
			if (t == 0) stn().set_tones(1200, 1400);
			else if (t == 1) stn().set_tones(2100, 2300);
			else if (t == 2) stn().set_tones(float(std::min(sh_.mark, sh_.space)), float(std::max(sh_.mark, sh_.space)));
			else stn().set_tones(1400, 1600);
		}
		return "*** AUDIO TONES: " + std::to_string(sh_.tones) + "\r\n";
	}
	if (abbrev(verb, "MARK", 3) || abbrev(verb, "SPACE", 3)) {
		const bool mk = verb[0] == 'M';
		int f = mk ? sh_.mark : sh_.space;
		if (!num(arg, 300, 3000, f)) return bad();
		(mk ? sh_.mark : sh_.space) = f;
		if (!arg.empty() && sh_.tones == 2) stn().set_tones(float(std::min(sh_.mark, sh_.space)), float(std::max(sh_.mark, sh_.space)));
		return std::string(mk ? "*** MARK" : "*** SPACE") + " FREQUENCY (HZ): " + std::to_string(f) + "\r\n";
	}
	if (abbrev(verb, "MODE", 2)) {
		int m = st().tx_mode();
		if (!num(arg, 0, 2, m)) return bad();
		if (!arg.empty()) st().set_tx_mode(m);
		return "*** PACTOR-TRANSMISSION-MODE (0=ASCII/1=HUFFMAN/2=PMC): " + std::to_string(st().tx_mode()) + "\r\n";
	}
	if (abbrev(verb, "MAXUP", 4)) return param(p.maxup, 2, 30, "*** SPEED-UP-PARAMETER: ");
	if (abbrev(verb, "MAXDOWN", 4)) return param(p.maxdown, 2, 30, "*** SPEED-DOWN-PARAMETER: ");
	if (abbrev(verb, "MAXTRY", 4)) return param(p.maxtry, 1, 9, "*** SPEED-UP-TRY-PARAMETER: ");
	if (abbrev(verb, "MAXSUM", 4)) return param(p.maxsum, 5, 60, "*** MEMO-ARQ-TIMEOUT: ");
	if (abbrev(verb, "MAXERROR", 4)) return param(p.maxerr, 30, 255, "*** TIMEOUT-PARAMETER: ", true);
	if (abbrev(verb, "FSKAMPL", 4)) return param(p.fska, 10, 1000, "AFSK TX AUDIO OUTPUT LEVEL (PEAK-TO-PEAK, mV): ");
	if (abbrev(verb, "PSKAMPL", 4)) return param(p.pska, 10, 1000, "PSK TX AUDIO PEAK LEVEL (PEAK-TO-PEAK, mV): ", true);
	if (abbrev(verb, "TXDELAY", 3)) {
		const std::string r = param(p.txdelay, 1, 31, "TX-DELAY: ", true);
		return r[0] == '\a' ? r : "TX-DELAY: " + std::to_string(p.txdelay) + "  (" + std::to_string(p.txdelay * 5) + " msec)\r\n";
	}
	if (abbrev(verb, "CSDELAY", 3)) {
		const std::string r = param(p.csdelay, 1, 31, "CS-DELAY: ");
		return r[0] == '\a' ? r : "CS-DELAY: " + std::to_string(p.csdelay) + "  (" + std::to_string(p.csdelay * 5) + " msec)\r\n";
	}
	if (abbrev(verb, "CYCLE", 2)) return param(sh_.cycle, 0, 2, "*** PACTOR-II CYCLE LENGTH (SHORT/LONG/AUTO): ");
	if (abbrev(verb, "CHOBELL", 4)) return param(p.chob, 0, 1, "*** CHANGEOVER-BELL-PARAMETER: ");
	if (abbrev(verb, "CHOCHR", 4)) return chr_param(p.chochr, "CHANGE-OVER-CHARACTER = ASCII: ", arg);
	if (abbrev(verb, "BKCHR", 2)) return chr_param(p.bkchr, "BREAK-IN-CHARACTER = ASCII: ", arg);
	if (abbrev(verb, "QRTCHR", 4)) return chr_param(p.qrtchr, "QRT-CHARACTER = ASCII: ", arg);
	if (abbrev(verb, "ESCCHR", 3)) return chr_param(p.escchr, "ESCAPE-CHARACTER = ASCII: ", arg);
	if (abbrev(verb, "REMOTE", 3)) return param(p.rem, 0, 1, "*** REMOTE: ");
	if (abbrev(verb, "CWID", 2)) {                   // kept, shown as the firmware does ("0  2" after CWID 0)
		if (!arg.empty()) { int a = 0; if (!num(arg.substr(0, arg.find(' ')), 0, 5, a)) return bad(); sh_.cwid = std::to_string(a) + "  2"; }
		return "*** CWID-PARAMETERS: " + sh_.cwid + "\r\n";
	}
	if (abbrev(verb, "BOX", 1)) return param(kbox_, 0, 1, "*** DIRECT BOX ACCESS: ");
	if (abbrev(verb, "ARX", 3)) return param(karx_, 0, 1, "*** AMTOR RECEPTION: ");
	if (abbrev(verb, "LFIGNORE", 2)) return param(klf_, 0, 2, "*** LF IGNORE PARAMETER: ");
	if (abbrev(verb, "BC", 2)) return param(kbc_, 0, 1, "*** FEC RECEPTION: ");
	if (abbrev(verb, "ADDLF", 2)) return param(kaddlf_, 0, 1, "*** TX AUTO LF INSERTION PARAMETER: ");
	if (abbrev(verb, "TR", 2)) return "*** COMMAND ONLY AVAILABLE ON AMTOR AND RTTY\r\n";
	if (abbrev(verb, "CONTYPE", 4)) return param(p.contype, 0, 3, "*** PACTOR RX CONNECT TYPE: ");
	if (abbrev(verb, "PTCHN", 4)) return param(p.hostch, 1, 31, "*** PACTOR HOSTMODE CHANNEL: ");
	if (abbrev(verb, "PDTIMER", 3)) {                // the firmware's texts and ranges
		int v = st().pdtimer();
		if (!num(arg, 2, 30, v)) return bad();
		if (!arg.empty()) stn().set_pduplex(st().pduplex(), v);
		return "*** PACTOR DUPLEX BREAKIN TIME: " + std::to_string(v) + "\r\n";
	}
	if (abbrev(verb, "PDUPLEX", 2)) {
		int v = st().pduplex() ? 1 : 0;
		if (!num(arg, 0, 1, v)) return bad();
		if (!arg.empty()) stn().set_pduplex(v != 0, st().pdtimer());
		return "*** PACTOR DUPLEX: " + std::to_string(v) + "\r\n";
	}
	if (abbrev(verb, "LISTEN", 1)) {
		int v = p.listen;
		if (!num(arg, 0, 5, v) || (v > 1 && v < 5)) return bad();
		p.listen = v;
		return "*** LISTEN-MODE: " + std::to_string(p.listen) + "\r\n";
	}
	if (abbrev(verb, "STATUS", 2)) {
		int v = p.status;
		if (!num(arg, 0, 2, v)) return bad();
		p.status = v;
		if (mode_ == TERMINAL && v == 2 && !status_auto_) { last_stb_ = status_byte(); stb_insert_ = std::string("\x1e") + char(last_stb_); }
		status_auto_ = v == 2;
		return "*** STATUS-LEVEL: " + std::to_string(p.status) + "\r\n";
	}
	if (abbrev(verb, "CPULOAD", 4)) {                // ours: is this computer keeping up (OK / BUSY / STARVED)
		return (sh_.cpu_status ? sh_.cpu_status() : std::string("*** CPU LOAD: NOT MEASURED (NO REAL-TIME AUDIO)")) + "\r\n";
	}
	if (abbrev(verb, "CPUADAPT", 3)) {               // ours (no command of the reference unit starts with CP)
		// Adaptive CPU features: 1 (default) = used, and switched off by level while this computer's
		// decodes cannot keep up; 0 = never used. For now one feature: long packets as P3 receiver.
		int v = stn().cpu_adaptive() ? 1 : 0;
		if (!num(arg, 0, 1, v)) return bad();
		if (!arg.empty()) stn().set_cpu_adaptive(v != 0);
		const int c = st().p3_long_ceiling();
		const std::string now = !v ? "P3 LONG PACKETS AS RECEIVER: NEVER" : c >= 6 ? "P3 LONG PACKETS AS RECEIVER: ALL LEVELS" :
		                        c > 0 ? "P3 LONG PACKETS AS RECEIVER: UP TO LEVEL " + std::to_string(c) + " (THIS COMPUTER)" : "P3 LONG PACKETS AS RECEIVER: NONE (THIS COMPUTER)";
		return "*** CPU-ADAPTIVE FEATURES (0=OFF/1=ADAPTIVE): " + std::to_string(v) + "  (" + now + ")\r\n";
	}
	if (abbrev(verb, "ICPOWER", 3)) {                // ours (no command of the reference unit starts with IC): ICPower [W]
		return (sh_.icpower ? sh_.icpower(arg) : std::string("*** IC-7300 POWER: NOT AVAILABLE IN THIS PROGRAM")) + "\r\n";
	}
	if (abbrev(verb, "TUNETONE", 2)) {               // ours (no command of the reference unit starts with TU): the tuning before a new call, TUnetone [S [HZ [DB]]] [auto|tone|radio]
		TuneTone &t = sh_.tune;
		if (!arg.empty()) {
			int v[3] = {t.secs, t.hz, t.db}, k = 0, m = t.method;
			const char *q = arg.c_str();
			while (*q) {                                 // up to three whole numbers and a method word (anywhere), nothing else
				while (*q == ' ') q++;
				if (!*q) break;
				const char *w = q;
				while (*q && *q != ' ') q++;
				const std::string tok(w, q);
				if (std::isalpha(static_cast<unsigned char>(tok[0]))) { if (!TuneTone::parse_method(tok, m)) return bad(); continue; }
				char *e = nullptr;
				const long x = std::strtol(tok.c_str(), &e, 10);
				if (e == tok.c_str() || *e || k == 3) return bad();
				v[k++] = int(std::max(-100000L, std::min(100000L, x)));
			}
			const int s = v[0], h = v[1], d = v[2];
			if (!TuneTone::valid_secs(s) || !TuneTone::valid_hz(h) || !TuneTone::valid_db(d)) return bad();
			t.secs = s; t.hz = h; t.db = d; t.method = m;
		}
		return t.text();
	}
	if (abbrev(verb, "PEERMARK", 4)) {               // ours (no command of the reference unit starts with PE): the Signum peer marker
		// 1 (default, owner's decision) = sent on P2/P3 links (from the next link on); 0 = not sent. The answer
		// also says what the other station of this link is (the front panel's Signum / Other lamps).
		int v = sh_.peer.marker ? 1 : 0;
		if (!num(arg, 0, 1, v)) return bad();
		sh_.peer.marker = v != 0;
		const std::string now = sh_.peer.link_text();
		return "*** PEER-MARKER: " + std::to_string(v) + (now.empty() ? std::string() : "  (" + now + ")") + "\r\n";
	}
	if (abbrev(verb, "ALCWARN", 3)) {                // ours (no command of the reference unit starts with AL): warn when the radio's ALC acts on P2/P3
		// 1 (default) = read the ALC meter through rigctld while we send P2/P3 and warn; 0 = off. Warns only.
		std::string a = arg;
		for (auto &c : a) c = char(std::toupper(static_cast<unsigned char>(c)));
		int v = sh_.alc.enabled ? 1 : 0;
		if (a == "ON") a = "1"; else if (a == "OFF") a = "0";
		if (!num(a, 0, 1, v)) return bad();
		sh_.alc.enabled = v != 0;
		return sh_.alc.text() + "\r\n";
	}
	if (abbrev(verb, "CMSG", 2)) {                   // the connect text on/off (LinBPQ: CMSG 0); the modem sends it as P3 callee
		int v = cmsg_;
		if (!num(arg, 0, 2, v)) return bad();
		if (!arg.empty()) { cmsg_ = v; stn().set_greeting(v ? std::string("\rSignum: P1/P2/P3 station\r") : std::string()); }
		return "*** CONNECT-MSG: " + std::to_string(cmsg_) + "\r\n";
	}
	if (verb == "U" || abbrev(verb, "UNPROTO", 1)) {   // U [mode | *repetitions] (manual 6.102; the reference firmware's own text)
		if (converse()) return "";                       // the reference unit answers nothing while a link or Unproto is up
		const size_t i = arg.find_first_not_of(' ');
		const std::string a = i == std::string::npos ? std::string() : arg.substr(i);
		if (!a.empty() && a[0] == '*') {                 // *1..5: only the repetition time (a digit right after the star; anything else only shows it)
			if (a.size() >= 2 && a[1] >= '1' && a[1] <= '5') sh_.up_reps = a[1] - '0';
			return "*** UNPROTO-REPETITION-TIME: " + std::to_string(sh_.up_reps) + "\r\n";
		}
		int m = sh_.up_mode;
		if (!a.empty()) {                                // a number up to the first space; the rest of the line is ignored
			size_t n = 0;
			while (n < a.size() && std::isdigit(uint8_t(a[n]))) n++;
			if (n == 0 || n > 3 || (n < a.size() && a[n] != ' ')) m = 0;
			else m = std::atoi(a.substr(0, n).c_str());
			if (!((m >= 1 && m <= 10) || (m >= 30 && m <= 41))) return bad();
		}
		if (sh_.unproto_refused) { const std::string r = sh_.unproto_refused(); if (!r.empty()) return r; }
		if (!st().start_unproto(m, sh_.up_reps)) return "*** ERROR: UNPROTO NOT POSSIBLE NOW\r\n";
		sh_.up_mode = m;
		static const char *const psk[] = {" DBPSK", " DQPSK", " 8-DPSK", " 16-DPSK"};
		const std::string d = m == 1 ? " FSK 100 BD" : m == 2 ? " FSK 200 BD" : m <= 10 ? std::string(psk[(m - 3) % 4]) + (m >= 7 ? "/LONG-CYCLE" : "/SHORT-CYCLE") : std::string();
		return "UNPROTO TRANSMISSION ACTIVE - MODE " + std::to_string(m) + ":" + d + " >>\r\n";
	}
	if (abbrev(verb, "TIME", 2)) return "Time:  " + clock(false) + "\r\n";   // the host's clock (TIME/DATE with an argument: accepted)
	if (abbrev(verb, "DATE", 2)) return "Date:   " + clock(true) + "\r\n";
	if (abbrev(verb, "VERSION", 1)) return sh_.version_text;
	if (abbrev(verb, "HELP", 1)) return help();
	return "";
}

std::string HostIface::help()
{
	return "Command key words:\r\n==================\r\n"
	       "ALCwarn   BKchr     CHOBell   CHOchr    CMsg      Connect   CONType   CPUadapt\r\n"
	       "CPULoad   CSDelay   CWid      CYcle     DAte      DD        Discon    ESCchr\r\n"
	       "FSKAmpl   Help      ICPower   Listen    MARk      MAXDown   MAXError  MAXSum\r\n"
	       "MAXTry    MAXUp     MOde      MYcall    MYLevel   PDTimer   PDuplex   PEERmark\r\n"
	       "PSKAmpl   PTChn     Qrt       QRTChr    REMote    RESTart   SPAce     STatus\r\n"
	       "TIme      TOnes     TUnetone  TXDelay   Unproto   Version   JHOST1    JHOST4\r\n"
	       "\r\nIn a link: ESC = command, Ctrl-D = QRT, Ctrl-Y = break-in (receiving) / changeover (sending)\r\n";
}

std::string HostIface::clock(bool date)
{
	const std::time_t now = std::time(nullptr);
	std::tm t;
	localtime_r(&now, &t);
	static const char *const day[7] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
	static const char *const mon[12] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
	char b[40];
	if (date) std::snprintf(b, sizeof b, "%s, %02d-%s-%02d", day[t.tm_wday], t.tm_mday, mon[t.tm_mon], t.tm_year % 100);
	else std::snprintf(b, sizeof b, "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
	return b;
}

uint8_t HostIface::status_byte() const
{
	const fmodem::P2Station &s = st();
	if (s.unproto()) return 0x80 | 0x40 | 0x08 | 0x02;
	const bool tune = sh_.tune.active();             // the tuning tone before our call: as the call's set-up
	if (!s.link_up() && !s.setting_up() && !tune) return par().listen ? 0xE7 : s.standby_status() == 0xF7 ? 0xF7 : 0x87;   // 0xF7: channel busy
	if (!s.link_up()) return 0x80 | 0x20 | (s.ptt() || sh_.tune.keyed() ? 0x08 : 0) | 0x06;   // set-up: SYNCH
	int stat = 3;
	const int te = s.led_traffic_err(), ir = s.led_idle_req();
	if (s.led_cho()) stat = 4;
	else if (te == 2) stat = 0;
	else if (ir == 2) stat = 1;
	else if (te == 1) stat = 2;
	return uint8_t(0x80 | 0x20 | (s.led_send() ? 0x08 : 0) | stat);
}

void HostIface::term_event(const LinkEvent &e)
{
	{
		if (e.up) {
			sh_.had_link = true;
			if (!st().caller())
				out_ += "\r\n*** MYCALL RECEIVED - AT  (" + std::to_string(st().link_plevel() == 1 && st().p1_baud() == 100 ? 100 : 200) + " Bd) - " + clock(false) + " / " + clock(true) + "\r\n";
			out_ += "*** CONNECTED TO " + e.call + "\r\n";   // (firmware: no blank line before it)
			linked_ = true;
		} else {
			const std::string why = st().link_note();
			if (!linked_ && local_qrt_) {
				// a call we aborted (ESC DD): only the stand-by text (firmware)
				linked_ = false; local_qrt_ = false; esc_cmd_ = false;
				out_ += stby_text() + "\r\ncmd: ";
				return;
			}
			if (!linked_) {
				// a call that never came up
				out_ += "\r\n*** NO RESPONSE FROM " + (sh_.calling.empty() ? e.call : sh_.calling) + " / " + clock(false) + " / " + clock(true) + "\r\n";
			} else if (why.find("timeout") != std::string::npos && !local_qrt_) {
				out_ += "\r\n*** TIMEOUT - DISCONNECTED AT - " + clock(false) + " / " + clock(true) + "\r\n";
			} else {
				// the firmware: "DISCONNECTED AT" on the side that asked for QRT,
				// "DISCONNECTED / QRT RECEIVED AT" on the other
				out_ += std::string("\r\n*** DISCONNECTED ") + (local_qrt_ ? "AT - " : "/ QRT RECEIVED AT - ") + clock(false) + " / " + clock(true) + "\r\n";
			}
			linked_ = false;
			local_qrt_ = false;
			out_ += "\r\n" + stby_text() + "\r\ncmd: ";
			esc_cmd_ = false;
		}
	}
}

void HostIface::host_byte(uint8_t b)
{
	if (mode_ == HOST_PLAIN) { plain_byte(b); return; }
	if (hdr_ < 2) { hdr_ = b == 0xAA ? hdr_ + 1 : 0; frame_.clear(); stuffed_ = false; return; }
	if (stuffed_) {                                    // after AA inside a frame
		stuffed_ = false;
		if (b == 0x00) { frame_.push_back(0xAA); goto check; }
		if (b == 0x55 && frame_.empty()) { resend(); hdr_ = 0; return; }   // the host asks for our last answer again
		frame_.clear(); hdr_ = b == 0xAA ? 2 : 0;      // resync: AA AA starts a new frame
		return;
	}
	if (b == 0xAA) { stuffed_ = true; return; }
	frame_.push_back(b);
check:
	if (frame_.size() >= 3 && frame_.size() == size_t(frame_[2]) + 1 + 3 + 2) {
		const size_t n = frame_.size() - 2;
		const uint16_t want = uint16_t(frame_[n] | frame_[n + 1] << 8);
		if (host_crc(frame_.data(), n) != want) {
			log("host frame CRC error");
			out_ += std::string("\xAA\xAA\xAA\x55", 4);
		} else {
			const uint8_t ch = frame_[0], ctl = frame_[1];
			const std::string pay(frame_.begin() + 3, frame_.begin() + long(n));
			const bool repeat = have_last_ && ch == last_ch_ && ctl == last_ctl_ && pay == last_payload_;
			if (repeat) {
				log("host frame repeated: ch " + std::to_string(ch) + ", last answer resent");
				resend();
			} else {
				last_ctl_ = ctl; last_ch_ = ch; have_last_ = true;
				last_payload_ = pay;
				handle(ch, (ctl & 1) != 0, pay);
			}
		}
		hdr_ = 0; frame_.clear();
	}
}

void HostIface::reply(uint8_t ch, uint8_t code, const std::string &payload)
{
	std::string f;
	f += char(ch); f += char(code); f += payload;
	if (mode_ == HOST_PLAIN) { last_reply_ = f; out_ += f; return; }
	const uint16_t c = host_crc(reinterpret_cast<const uint8_t *>(f.data()), f.size());
	f += char(c & 0xff); f += char(c >> 8);
	std::string s = "\xAA\xAA";
	for (char x : f) { s += x; if (uint8_t(x) == 0xAA) s += '\0'; }
	last_reply_ = s;
	out_ += s;
}

void HostIface::handle(uint8_t ch, bool cmd, const std::string &p)
{
	const uint8_t pch = uint8_t(hostch());
	if (!cmd) {                                        // data
		if (ch == pch) { link_send(p); log("data " + std::to_string(p.size()) + " bytes"); }
		reply(ch, 0, "");
		return;
	}
	std::string verb, arg;
	split(p, verb, arg);
	if (verb != "L" && verb != "G") log("host cmd ch " + std::to_string(ch) + ": " + p);
	if (!p.empty() && p[0] == '#') { hash_cmd(ch, p.substr(1)); return; }
	if (verb == "JHOST0") { reply(ch, 0, ""); mode_ = TERMINAL; line_.clear(); log("leave host mode"); return; }
	if (ch == 255 && verb == "G") {                    // the channels with something to fetch
		std::string s;
		if (!q_.empty()) s += char(pch + 1);
		uint8_t stb[4];
		st().host_status(stb);
		if (stn().quiet() || st().unproto()) stb[0] = status_byte();
		if (std::string(reinterpret_cast<char *>(stb), 4) != last_status_) s += char(254 + 1);
		reply(ch, 1, s + '\0');
		return;
	}
	if (ch == 254 && !verb.empty() && verb[0] == 'G') {   // the status: G one byte, G3 the 4-byte block (LinBPQ) (firmware: 00 E7 / 03 E7 00 01 80)
		uint8_t stb[4] = {0, 0, 1, 128};
		st().host_status(stb);
		if (stn().quiet() || st().unproto()) stb[0] = status_byte();      // idle: the status byte (firmware E7 00 01 80)
		last_status_.assign(reinterpret_cast<char *>(stb), 4);
		if (verb == "G") reply(ch, 7, std::string(1, char(0)) + last_status_.substr(0, 1));
		else reply(ch, 7, std::string(1, char(3)) + last_status_);
		return;
	}
	if (ch == pch && verb == "I" && arg.empty()) { reply(ch, 1, st().mycall() + '\0'); return; }
	if (ch == pch && verb.size() > 1 && verb[0] == 'I' && is_call(verb.substr(1))) { st().set_mycall(verb.substr(1)); reply(ch, 0, ""); return; }
	if (ch == pch && verb == "I" && is_call(arg)) { st().set_mycall(upper_str(arg)); reply(ch, 0, ""); return; }
	if (verb == "%L" && ch == pch) { reply(ch, 1, std::string("1") + '\0'); return; }      // (firmware: "1")
	// %O: changeover as sender, break-in as receiver; %I: break-in. Every mode;
	// answered 0 like every accepted command [inf: not captured].
	if ((verb == "%O" || verb == "%I") && ch == pch) {
		st().changeover_key(verb == "%I" ? fmodem::P1Station::KEY_BK : fmodem::P1Station::KEY_BK | fmodem::P1Station::KEY_CHO);
		reply(ch, 0, "");
		return;
	}
	if (verb == "@T2" && ch == pch) { reply(ch, 1, std::string("500") + '\0'); return; }   // (firmware: "500")
	if (verb == "%T" && ch == pch) { reply(ch, 1, std::to_string(st().tx_unacked()) + '\0'); return; }
	if (verb == "@B" && ch == pch) { reply(ch, 1, std::to_string(968 - int(std::min<size_t>((st().tx_unsent() + held()) / 32, 900))) + '\0'); return; }
	if (verb == "L") {
		int stat = 0, frames = 0;
		if (ch == pch) for (const auto &it : q_) (it.code == 7 ? frames : stat)++;
		const size_t unsent = st().tx_backlog() + held();
		// 4 while the link is up, also during our own orderly QRT (the firmware reports
		// 6/5 there, never 3: Pat's driver takes 3 -> 4 after its own D as a fresh connect)
		const int state = st().link_up() ? 4 : st().setting_up() || sh_.tune.active() ? 1 : 0;
		const std::string s = std::to_string(stat) + " " + std::to_string(frames) + " " + std::to_string((unsent + 255) / 256) + " " +
		                      std::to_string(st().tx_unacked() ? 1 : 0) + " 0 " + std::to_string(ch == pch ? state : 0);
		reply(ch, 1, s + '\0');
		return;
	}
	if (verb == "G") {
		if (ch != pch || q_.empty()) { reply(ch, 0, ""); return; }
		const Item it = q_.front(); q_.pop_front();
		if (it.code == 7) reply(ch, 7, std::string(1, char(it.text.size() - 1)) + it.text);
		else reply(ch, uint8_t(it.code), it.text + '\0');
		return;
	}
	if (verb == "C" && ch == pch) {
		if (st().link_up() || st().setting_up() || sh_.tune.active()) { reply(ch, 2, std::string("CHANNEL ALREADY CONNECTED") + '\0'); return; }
		std::string c = arg.substr(0, arg.find(' '));
		for (auto &x : c) x = char(std::toupper(uint8_t(x)));
		if (c.empty()) { reply(ch, 2, std::string("INVALID CALLSIGN") + '\0'); return; }
		sh_.last_call = c; sh_.calling = c;
		sh_.place_call(c);
		reply(ch, 0, "");
		return;
	}
	if ((verb == "D" || verb == "DD") && ch == pch) {
		if (!st().link_up() && !st().setting_up() && !st().unproto() && !sh_.tune.active()) { reply(ch, 1, std::string("CHANNEL NOT CONNECTED") + '\0'); return; }   // (firmware)
		if (sh_.tune.cancel()) {}                    // the tuning tone before our call: aborted, the call dropped
		else if (verb == "DD" || arg == "D") st().abort_link(); else link_qrt();
		reply(ch, 0, "");
		return;
	}
	if (verb == "U") {      // host mode: the connect text (manual 10.4.15; the reference unit): "U" asks (so does any other argument), "U 0|1|2 [text]" sets
		const size_t i = arg.find_first_not_of(' ');
		const std::string a = i == std::string::npos ? std::string() : arg.substr(i);
		if (!a.empty() && a[0] >= '0' && a[0] <= '2' && (a.size() == 1 || a[1] == ' ')) {
			const size_t t = a.find_first_not_of(' ', 1);
			sh_.ctext = t == std::string::npos ? std::string() : a.substr(t);
			reply(ch, 0, "");
		} else reply(ch, 1, sh_.ctext + '\0');
		return;
	}
	// The terminal's parameter commands: a value set is answered 0; a query its text (code 1);
	// a bad value or an unknown command code 2.
	const std::string t = general(verb, arg);
	if (t.empty()) { reply(ch, 2, std::string("INVALID COMMAND: ") + verb + '\0'); return; }
	if (t[0] == '\a') { reply(ch, 2, strip_crlf(t) + '\0'); return; }
	if (!arg.empty()) { reply(ch, 0, ""); return; }
	reply(ch, 1, strip_crlf(t) + '\0');
}

void HostIface::hash_cmd(uint8_t ch, std::string l)
{
	while (!l.empty() && (l.back() == '\r' || l.back() == '\n')) l.pop_back();
	std::string verb, arg;
	split(l, verb, arg);
	const bool busy = st().link_up() || st().setting_up() || st().unproto() || sh_.tune.active();
	if (verb.empty() || verb.compare(0, 5, "JHOST") == 0) {}
	else if (verb == "DD" || abbrev(verb, "DISCONNECT", 1) || abbrev(verb, "QRT", 1)) {
		if (busy && !sh_.tune.cancel()) { if (verb == "DD") st().abort_link(); else link_qrt(); }
	} else if (abbrev(verb, "CONNECT", 1)) {
		std::string c = arg.empty() ? sh_.last_call : arg.substr(0, arg.find(' '));
		for (auto &x : c) x = char(std::toupper(uint8_t(x)));
		if (!c.empty() && !busy) { sh_.last_call = c; sh_.calling = (c.size() > 1 && (c[0] == '!' || c[0] == ';')) ? c.substr(1) : c; sh_.place_call(c); }
	} else (void)general(verb, arg);
	reply(ch, 0, "");
}

void HostIface::split(const std::string &l, std::string &verb, std::string &arg)
{
	size_t a = l.find_first_not_of(' ');
	if (a == std::string::npos) { verb.clear(); arg.clear(); return; }
	size_t b = l.find(' ', a);
	verb = l.substr(a, b == std::string::npos ? std::string::npos : b - a);
	for (auto &c : verb) c = char(std::toupper(uint8_t(c)));
	const size_t c = b == std::string::npos ? std::string::npos : l.find_first_not_of(' ', b);
	arg = c == std::string::npos ? "" : l.substr(c);
	while (!arg.empty() && arg.back() == ' ') arg.pop_back();
}

}  // namespace signum

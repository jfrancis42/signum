// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// start_config.hpp: the settings file, ~/.config/signum/signum.conf
// ($XDG_CONFIG_HOME/signum/signum.conf; SIGNUM_CONFIG=FILE another, SIGNUM_CONFIG=none
// none; --config FILE, --no-config on the command line). One "option = value" per line,
// the option a long command-line option without its "--"; flags take on/off. Read at start,
// before the command line, which therefore wins. The GUI's Configure tab writes it (the
// settings that apply at the next start, and "keep at next start" for the live ones):
// a changed option's line is replaced in place, comments and other lines are kept, the
// file is written 0600 (it may hold logins). docs/md/options.md "The settings file".
#pragma once
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace signum {

struct StartOpt {
	enum Kind { TEXT, PATH, INT, NUM, FLAG, CHOICE, CALL, TONES, PTT, WSADDR, CRED, SECRET };
	const char *key;
	Kind kind;
	double lo, hi;                      // INT, NUM
	const char *choices;                // CHOICE: "a|b|c"
};

// Every option the settings file takes (the command line's, without the test and
// one-shot ones: --audio-in/--audio-out/--pace/--duration/--rx-noise, --rts (= ptt),
// --web-auth-file/--ws-auth-file (the logins themselves are here), --version, --help).
inline const std::vector<StartOpt> &start_opts()
{
	static const std::vector<StartOpt> t = {
	    {"call", StartOpt::CALL, 0, 0, nullptr},
	    {"mylevel", StartOpt::INT, 1, 3, nullptr},
	    {"tones", StartOpt::TONES, 300, 3000, nullptr},
	    {"listen", StartOpt::CHOICE, 0, 0, "on|off"},
	    {"p3-unproto-rx", StartOpt::FLAG, 0, 0, nullptr},
	    {"peer-marker", StartOpt::CHOICE, 0, 0, "on|off"},
	    {"alc-warn", StartOpt::CHOICE, 0, 0, "on|off"},
	    {"cpu-adaptive", StartOpt::CHOICE, 0, 0, "on|off"},
	    {"pduplex", StartOpt::CHOICE, 0, 0, "on|off"},
	    {"pdtimer", StartOpt::INT, 2, 30, nullptr},
	    {"tune-tone", StartOpt::INT, 0, 10, nullptr},
	    {"tune-method", StartOpt::CHOICE, 0, 0, "auto|tone|radio"},
	    {"tune-hz", StartOpt::INT, 300, 2700, nullptr},
	    {"tune-db", StartOpt::INT, -30, 0, nullptr},
	    {"alsa", StartOpt::TEXT, 0, 0, nullptr},
	    {"ptt", StartOpt::PTT, 0, 0, nullptr},
	    {"max-key", StartOpt::NUM, 1, 600, nullptr},
	    {"tx-gain", StartOpt::NUM, -40, 20, nullptr},
	    {"cpu-clock", StartOpt::CHOICE, 0, 0, "auto|max|off"},
	    {"cal", StartOpt::PATH, 0, 0, nullptr},
	    {"no-autocal", StartOpt::FLAG, 0, 0, nullptr},
	    {"autocal-state", StartOpt::PATH, 0, 0, nullptr},
	    {"autocal-max-ms", StartOpt::NUM, 1, 1000, nullptr},
	    {"audio-ppm", StartOpt::NUM, -2000, 2000, nullptr},
	    {"rt-latency", StartOpt::NUM, 0, 2, nullptr},
	    {"prime-ms", StartOpt::NUM, 5, 500, nullptr},
	    {"pty", StartOpt::PATH, 0, 0, nullptr},
	    {"telnet", StartOpt::INT, 0, 65535, nullptr},
	    {"telnet-bind", StartOpt::TEXT, 0, 0, nullptr},
	    {"telnet-auth", StartOpt::SECRET, 0, 0, nullptr},
	    {"ws", StartOpt::WSADDR, 0, 0, nullptr},
	    {"no-ws", StartOpt::FLAG, 0, 0, nullptr},
	    {"web-auth", StartOpt::CRED, 0, 0, nullptr},
	    {"ws-auth", StartOpt::CRED, 0, 0, nullptr},
	    {"gui", StartOpt::PATH, 0, 0, nullptr},
	    {"quiet", StartOpt::FLAG, 0, 0, nullptr},
#if defined(SIGNUM_CRYPTO) && !defined(SIGNUM_NO_CRYPTO)
	    // encryption (docs/md/encryption.md); none of these exist in a build without encryption (the default)
	    {"encryption", StartOpt::CHOICE, 0, 0, "on|verify|off"},
	    {"encryption-ack", StartOpt::CHOICE, 0, 0, "NOT-AMATEUR"},
	    {"encryption-trust", StartOpt::CHOICE, 0, 0, "stored|tofu|verified"},
	    {"verify-trust", StartOpt::CHOICE, 0, 0, "stored|tofu|verified"},
	    {"sign", StartOpt::CHOICE, 0, 0, "on|off"},
	    {"encryption-wait", StartOpt::NUM, 10, 600, nullptr},
	    {"encryption-compression", StartOpt::CHOICE, 0, 0, "on|off"},
	    {"key-dir", StartOpt::PATH, 0, 0, nullptr},
#endif
	};
	return t;
}

class StartConfig {
public:
	std::string path;                   // "" = no settings file this run
	bool read = false;                  // the file existed and was read at start
	std::vector<std::pair<std::string, std::string>> values;   // option -> value, as the file holds them now
	std::vector<std::string> warnings;

	static std::string default_path()
	{
		if (const char *e = std::getenv("SIGNUM_CONFIG")) return !std::strcmp(e, "none") ? std::string() : std::string(e);
		const char *x = std::getenv("XDG_CONFIG_HOME"), *h = std::getenv("HOME");
		return x && *x ? std::string(x) + "/signum/signum.conf" : h ? std::string(h) + "/.config/signum/signum.conf" : "";
	}
	static const StartOpt *find(const std::string &k)
	{
		for (const auto &o : start_opts()) if (k == o.key) return &o;
		return nullptr;
	}
	static bool flag_value(const std::string &v, bool &on)
	{
		if (v == "on" || v == "1" || v == "yes" || v == "true") { on = true; return true; }
		if (v == "off" || v == "0" || v == "no" || v == "false") { on = false; return true; }
		return false;
	}
	// An option's value checked as the command line would take it: "" if good, else why not.
	// ptt_ok: the PTT spec parser (radio::PttSpec::parse), given by the program.
	template <class PttOk>
	static std::string check(const StartOpt &o, const std::string &v, PttOk ptt_ok)
	{
		char *e = nullptr;
		switch (o.kind) {
		case StartOpt::TEXT: case StartOpt::PATH:
			if (v.empty()) return "empty";
			if (o.kind == StartOpt::PATH && std::string(o.key) == "pty" && v != "none" && v[0] != '/') return "an absolute path, or none";
			return "";
		case StartOpt::INT: case StartOpt::NUM: {
			const double d = std::strtod(v.c_str(), &e);
			if (e == v.c_str() || *e || !std::isfinite(d)) return "not a number";
			if (o.kind == StartOpt::INT && d != std::floor(d)) return "not a whole number";
			if (d < o.lo || d > o.hi) { char b[80]; std::snprintf(b, sizeof b, "out of range (%g to %g)", o.lo, o.hi); return b; }
			return "";
		}
		case StartOpt::FLAG: { bool on; return flag_value(v, on) ? "" : "on or off"; }
		case StartOpt::CHOICE: {
			const std::string c = std::string("|") + o.choices + "|";
			return !v.empty() && v.find('|') == std::string::npos && c.find("|" + v + "|") != std::string::npos ? "" : std::string("one of ") + o.choices;
		}
		case StartOpt::CALL:
			if (v.empty() || v.size() > 9) return "a call sign (up to 9 characters)";
			for (char c : v) if (!std::isalnum(uint8_t(c)) && c != '-' && c != '/') return "a call sign (letters, digits, - and /)";
			return "";
		case StartOpt::TONES: {
			float lo = 0, hi = 0; char x;
			if (std::sscanf(v.c_str(), "%f,%f%c", &lo, &hi, &x) != 2 || lo < o.lo || hi > o.hi || hi <= lo) return "LO,HI in Hz (300-3000, LO below HI)";
			return "";
		}
		case StartOpt::PTT: return ptt_ok(v) ? "" : "rts:TTY, dtr:TTY, civ:TTY:ADDR[:BAUD], rig[:HOST[:PORT]], vox or none";
		case StartOpt::WSADDR: {
			const size_t c = v.rfind(':');
			const std::string port = c == std::string::npos ? v : v.substr(c + 1);
			const long p = std::strtol(port.c_str(), &e, 10);
			if (port.empty() || *e || p <= 0 || p > 65535 || (c != std::string::npos && c == 0)) return "[ADDR:]PORT";
			return "";
		}
		case StartOpt::CRED: return v.find(':') != std::string::npos && v.front() != ':' ? "" : "USER:PASS";
		case StartOpt::SECRET: return v.empty() ? "empty" : "";
		}
		return "";
	}

	// Read the file (a missing file is no error). Unknown options and bad lines are warnings.
	bool load()
	{
		values.clear();
		read = false;
		if (path.empty()) return false;
		FILE *f = std::fopen(path.c_str(), "r");
		if (!f) return false;
		read = true;
		char line[1024];
		int n = 0;
		while (std::fgets(line, sizeof line, f)) {
			n++;
			std::string k, v;
			if (!split(line, k, v)) continue;
			if (!find(k)) { warnings.push_back(path + ":" + std::to_string(n) + ": unknown option '" + k + "' (ignored)"); continue; }
			put(k, v);
		}
		std::fclose(f);
		return true;
	}
	// The file's options as command-line arguments (flags that are off give none).
	std::vector<std::string> args() const
	{
		std::vector<std::string> a;
		for (const auto &kv : values) {
			const StartOpt *o = find(kv.first);
			if (!o) continue;
			if (o->kind == StartOpt::FLAG) { bool on = false; if (flag_value(kv.second, on) && on) a.push_back("--" + kv.first); continue; }
			a.push_back("--" + kv.first);
			a.push_back(kv.second);
		}
		return a;
	}
	std::string get(const std::string &k) const
	{
		for (const auto &kv : values) if (kv.first == k) return kv.second;
		return "";
	}
	bool has(const std::string &k) const
	{
		for (const auto &kv : values) if (kv.first == k) return true;
		return false;
	}
	// Set (value non-empty) or remove (value empty) one option in the file, keeping every
	// other line. "" or why not.
	template <class PttOk>
	std::string set(const std::string &k, const std::string &v, PttOk ptt_ok)
	{
		if (path.empty()) return "no settings file this run (--no-config or SIGNUM_CONFIG=none)";
		const StartOpt *o = find(k);
		if (!o) return "unknown option " + k;
		if (v.find_first_of("\r\n") != std::string::npos) return k + ": one line only";
		if (!v.empty()) { const std::string why = check(*o, v, ptt_ok); if (!why.empty()) return k + ": " + why; }
		std::vector<std::string> lines;
		if (FILE *f = std::fopen(path.c_str(), "r")) {
			char line[1024];
			while (std::fgets(line, sizeof line, f)) { std::string s = line; while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back(); lines.push_back(s); }
			std::fclose(f);
		} else {
			lines.push_back("# Signum settings, read at start; command-line options override them.");
			lines.push_back("# One \"option = value\" per line: the long command-line options without \"--\";");
			lines.push_back("# flags take on or off. Written by the GUI's Configure tab too. docs/md/options.md.");
		}
		bool done = false;
		std::vector<std::string> out;
		for (const auto &l : lines) {
			std::string lk, lv;
			if (split(l.c_str(), lk, lv) && lk == k) {
				if (!done && !v.empty()) out.push_back(k + " = " + v);
				done = true;
				continue;
			}
			out.push_back(l);
		}
		if (!done && !v.empty()) out.push_back(k + " = " + v);
		const size_t sl = path.rfind('/');
		if (sl != std::string::npos && sl > 0) ::mkdir(path.substr(0, sl).c_str(), 0755);
		const std::string tmp = path + ".tmp";
		const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
		FILE *f = fd < 0 ? nullptr : ::fdopen(fd, "w");
		if (!f) { if (fd >= 0) ::close(fd); return std::string("cannot write ") + tmp + ": " + std::strerror(errno); }
		for (const auto &l : out) { std::fputs(l.c_str(), f); std::fputc('\n', f); }
		const bool ok = std::fflush(f) == 0 && ::fsync(fd) == 0;
		std::fclose(f);
		if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) { ::unlink(tmp.c_str()); return "cannot write " + path; }
		if (v.empty()) erase(k); else put(k, v);
		return "";
	}

private:
	static bool split(const char *line, std::string &k, std::string &v)
	{
		std::string s = line;
		while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
		size_t i = s.find_first_not_of(" \t");
		if (i == std::string::npos || s[i] == '#') return false;
		const size_t eq = s.find('=', i);
		if (eq == std::string::npos) return false;
		k = s.substr(i, eq - i);
		while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
		if (k.size() > 2 && k[0] == '-' && k[1] == '-') k = k.substr(2);
		const size_t j = s.find_first_not_of(" \t", eq + 1);
		v = j == std::string::npos ? std::string() : s.substr(j);
		return !k.empty();
	}
	void put(const std::string &k, const std::string &v)
	{
		for (auto &kv : values) if (kv.first == k) { kv.second = v; return; }
		values.emplace_back(k, v);
	}
	void erase(const std::string &k)
	{
		for (auto it = values.begin(); it != values.end(); ++it) if (it->first == k) { values.erase(it); return; }
	}
};

}  // namespace signum

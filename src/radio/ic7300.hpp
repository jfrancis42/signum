// ic7300.hpp: zero-effort set-up of an Icom IC-7300 for Signum (docs/md/radio-ic7300.md "Zero-effort set-up").
//
//   find()      the radios: the USB serial port (/dev/serial/by-id/*IC-7300*, a CP210x whose USB serial
//               string names the IC-7300) and the USB Audio CODEC behind the same USB hub (the radio's own
//               hub inside it), matched in sysfs
//   Link        CI-V over the radio's USB serial port, opened by util::SafePort: RTS and DTR low throughout,
//               the port used and left at speed B0 (src/util/serial_safe.hpp)
//   probe()     the CI-V rate and address: at B0 the chip keeps its last rate; the radio's ID (19 00) is asked
//               there, and the rate is then measured by timing (a run of preamble bytes FE, which the radio
//               skips, before the question). Only a port not at B0 (the first open after plug-in: the kernel
//               already raised the lines) is swept from rate to rate (no line moves). A radio that does not
//               answer at the port's rate needs one step out of B0 (the driver raises RTS/DTR for about a
//               millisecond): only after the user's confirmation, and never with USB SEND set to key on them
//               (unknown then: the warning says so)
//   plan()      every item read: what it is, what Signum sets (Profile::SIGNUM) or what an external modem on the
//               ACC socket needs (Profile::EXTERNAL: the same items; only DATA MOD, USB SEND and USB MOD Level differ)
//   setup()     the backup file first, then each write read back and compared
//   restore()   a backup file written back, each item read back
//
// Never written by the set-ups: RF power (14 0A) and the frequency (05 / 00); they are read and recorded only.
// RF POWER is written by one action only, --ic7300-power (src/radio/ic7300_power.hpp), whose backup is marked
// "restores_rf_power = yes" so that a restore of that backup (and only of such a backup) puts RF POWER back.
// Nothing here keys the radio: no "TX on" (1C 00 01) is ever sent; the TX state is read (1C 00) before anything
// is written, and a radio that is transmitting is left alone (ic7300_power.hpp's --measure is the one exception).
//
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
#pragma once

#include "../util/serial_safe.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <string>
#include <vector>

namespace radio {
namespace ic7300 {

using Bytes = std::vector<uint8_t>;

inline std::string hex(const Bytes &b)
{
	std::string s;
	char x[4];
	for (size_t i = 0; i < b.size(); i++) { std::snprintf(x, sizeof x, "%s%02X", i ? " " : "", b[i]); s += x; }
	return s;
}
bool unhex(const std::string &s, Bytes &out);
inline int bcd(const Bytes &b)                         // big-endian BCD (the 1A 05 levels: 0000-0255)
{
	int v = 0;
	for (uint8_t x : b) { if ((x >> 4) > 9 || (x & 15) > 9) return -1; v = v * 100 + (x >> 4) * 10 + (x & 15); }
	return v;
}
inline Bytes to_bcd2(int v) { return {uint8_t((v / 1000 % 10) << 4 | (v / 100 % 10)), uint8_t((v / 10 % 10) << 4 | (v % 10))}; }

inline double mono()
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return double(ts.tv_sec) + double(ts.tv_nsec) * 1e-9;
}

// ---- finding the radios ---------------------------------------------------------------------------
struct Radio {
	std::string port;        // /dev/serial/by-id/... (stable: names the radio's serial number)
	std::string dev;         // /dev/ttyUSBn
	std::string serial;      // the radio's serial number ("02019380")
	std::string card;        // the codec's ALSA card id ("CODEC"; "" = none found on the radio's hub)
	std::string alsa;        // plughw:CARD=<card>
	std::string hub;         // the radio's USB hub in sysfs (the serial chip and the codec both sit on it)
};

namespace detail {
inline std::string slurp(const std::string &path)
{
	FILE *f = std::fopen(path.c_str(), "r");
	if (!f) return "";
	char b[256] = {0};
	const bool ok = std::fgets(b, sizeof b, f) != nullptr;
	std::fclose(f);
	std::string s = ok ? b : "";
	while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
	return s;
}
inline std::string real(const std::string &p)
{
	char r[PATH_MAX];
	return ::realpath(p.c_str(), r) ? std::string(r) : std::string();
}
inline std::string parent(const std::string &p) { const size_t k = p.rfind('/'); return k == std::string::npos || k == 0 ? std::string() : p.substr(0, k); }
inline std::string base(const std::string &p) { const size_t k = p.rfind('/'); return k == std::string::npos ? p : p.substr(k + 1); }
// The USB device a sysfs path lies in: the nearest ancestor (or itself) with an idVendor file.
inline std::string usb_device(std::string p)
{
	for (int i = 0; i < 8 && !p.empty(); i++, p = parent(p)) {
		struct stat st{};
		if (::stat((p + "/idVendor").c_str(), &st) == 0) return p;
	}
	return "";
}
inline std::vector<std::string> list(const std::string &dir)
{
	std::vector<std::string> v;
	if (DIR *d = ::opendir(dir.c_str())) {
		while (dirent *e = ::readdir(d)) if (e->d_name[0] != '.') v.push_back(e->d_name);
		::closedir(d);
	}
	std::sort(v.begin(), v.end());
	return v;
}
}  // namespace detail

// Every IC-7300 on this computer. root: a prefix for /dev and /sys (tests: a fake tree).
std::vector<Radio> find(const std::string &root = "");

// ---- CI-V over the radio's USB serial port -----------------------------------------------------
class Link {
public:
	enum Res { OK, NG, NONE };
	explicit Link(util::SafePort &p, uint8_t addr = 0x94) : port_(p), addr_(addr) {}
	void addr(uint8_t a) { addr_ = a; }
	uint8_t addr() const { return addr_; }
	double wait_s = 0.30;     // per try
	int tries = 4;            // another program on the port (a guard polling it) can read an answer first

	// A query: the data after the command bytes of the radio's answer.
	bool read(const Bytes &cmd, Bytes &data)
	{
		for (int t = 0; t < tries; t++) {
			drain();
			if (!send(cmd)) return false;
			const double end = mono() + wait_s;
			Bytes f;
			while (next(f, end)) {
				if (f.size() < cmd.size() || !std::equal(cmd.begin(), cmd.end(), f.begin())) continue;   // another program's answer
				data.assign(f.begin() + long(cmd.size()), f.end());
				return true;
			}
		}
		return false;
	}
	// A query answered the same twice in a row (up to 8 answers). Another program reading the same port (a
	// guard polling the radio, a station's PTT check) takes bytes out of the stream at random, and what is
	// left can join into a well-formed frame with the wrong data (seen on the bench: an ON read as OFF).
	bool read_sure(const Bytes &cmd, Bytes &data)
	{
		Bytes prev, cur;
		bool have = false;
		for (int k = 0; k < 8; k++) {
			if (!read(cmd, cur)) { if (!have) return false; continue; }
			if (have && cur == prev) { data = cur; return true; }
			prev = cur; have = true;
		}
		return false;
	}
	// A setting: the radio's OK (FB) or NG (FA).
	Res write(const Bytes &cmd, const Bytes &data)
	{
		Bytes c = cmd;
		c.insert(c.end(), data.begin(), data.end());
		for (int t = 0; t < tries; t++) {
			drain();
			if (!send(c)) return NONE;
			const double end = mono() + wait_s;
			Bytes f;
			while (next(f, end)) {
				if (f.size() == 1 && f[0] == 0xFB) return OK;
				if (f.size() == 1 && f[0] == 0xFA) return NG;
			}
		}
		return NONE;
	}
	// The radio's ID (19 00) to the broadcast address: its CI-V address, 0 = no answer.
	uint8_t ask_address()
	{
		const uint8_t keep = addr_;
		for (int t = 0; t < tries; t++) {
			drain();
			const uint8_t q[] = {0xFE, 0xFE, 0x00, 0xE0, 0x19, 0x00, 0xFD};
			if (port_.write(q, sizeof q) != ssize_t(sizeof q)) return 0;
			const double end = mono() + wait_s;
			Bytes f;
			addr_ = 0;                                   // any sender
			while (next(f, end))
				if (f.size() == 3 && f[0] == 0x19 && f[1] == 0x00) { addr_ = keep; return f[2]; }
			addr_ = keep;
		}
		return 0;
	}
	// Seconds from the question to the answer, with n preamble bytes (FE) before the question; < 0: none.
	double timed_id(int n)
	{
		drain();
		Bytes q(size_t(n), 0xFE);
		const uint8_t t[] = {0xFE, 0xFE, addr_, 0xE0, 0x19, 0x00, 0xFD};
		q.insert(q.end(), t, t + sizeof t);
		const double t0 = mono();
		if (port_.write(q.data(), q.size()) != ssize_t(q.size())) return -1;
		Bytes f;
		const double end = t0 + 0.4 + n * 10.0 / 2400.0;
		while (next(f, end)) if (f.size() == 3 && f[0] == 0x19 && f[1] == 0x00) return mono() - t0;
		return -1;
	}

private:
	util::SafePort &port_;
	uint8_t addr_;
	Bytes buf_;

	bool send(const Bytes &body)
	{
		Bytes q = {0xFE, 0xFE, addr_, 0xE0};
		q.insert(q.end(), body.begin(), body.end());
		q.push_back(0xFD);
		return port_.write(q.data(), q.size()) == ssize_t(q.size());
	}
	void drain() { uint8_t d[512]; while (port_.read(d, sizeof d) > 0) {} buf_.clear(); }
	// The next frame from the radio to us (FE FE E0 ADDR ... FD): its body. Our own commands echoed back
	// (to the radio), broadcasts (to 00) and frames to other addresses are skipped.
	bool next(Bytes &body, double end)
	{
		for (;;) {
			for (;;) {                                   // a whole frame in the buffer?
				size_t s = 0;
				while (s + 1 < buf_.size() && !(buf_[s] == 0xFE && buf_[s + 1] == 0xFE)) s++;
				if (s + 1 >= buf_.size()) { if (!buf_.empty() && buf_.back() != 0xFE) buf_.clear(); else if (buf_.size() > 1) buf_.erase(buf_.begin(), buf_.end() - 1); break; }
				size_t h = s;
				while (h < buf_.size() && buf_[h] == 0xFE) h++;   // any number of preamble bytes
				size_t e = h;
				while (e < buf_.size() && buf_[e] != 0xFD && buf_[e] != 0xFE) e++;
				if (e >= buf_.size()) { buf_.erase(buf_.begin(), buf_.begin() + long(s)); break; }
				if (buf_[e] == 0xFE) { buf_.erase(buf_.begin(), buf_.begin() + long(e)); continue; }   // a broken frame (bytes read by another program): dropped
				Bytes f(buf_.begin() + long(h), buf_.begin() + long(e));
				buf_.erase(buf_.begin(), buf_.begin() + long(e) + 1);
				if (f.size() >= 3 && f[0] == 0xE0 && (addr_ == 0 || f[1] == addr_)) { body.assign(f.begin() + 2, f.end()); return true; }
			}
			if (mono() >= end) return false;
			uint8_t d[512];
			const ssize_t n = port_.read(d, sizeof d);
			if (n > 0) buf_.insert(buf_.end(), d, d + n);
			else ::usleep(1000);
		}
	}
};

// ---- the CI-V rate ---------------------------------------------------------------------------------
// The IC-7300's CI-V USB Baud Rate choices, the factory value first.
inline const std::vector<int> &rates() { static const std::vector<int> r = {115200, 9600, 19200, 38400, 57600, 4800}; return r; }

struct Probe {
	bool answered = false;
	uint8_t addr = 0;
	int baud = 0;               // 0 = unknown (it answers at the port's rate, which could not be measured)
	std::string how;            // how the rate was found
	bool was_b0 = true;         // the port was at B0 at the open (nothing raised)
	bool needs_b0_exit = false; // no answer at the port's rate: finding it needs a step out of B0 (raises RTS/DTR)
	std::string note;
};

// The rate by timing: n preamble bytes take n * 10 / rate s on the wire. Measured twice with 600 and once
// without, the nearest of the radio's rates within 25 %, else 0.
int measure_rate(Link &l, std::string &how);

// Open the port (lines low, B0) and find the radio's address and rate. allow_b0_exit: may leave B0 to
// sweep the rates (raises RTS/DTR for an instant). The port stays open, at B0, at the radio's rate when
// it answered.
bool probe(util::SafePort &p, const std::string &path, Probe &pr, bool allow_b0_exit, const std::function<void(const std::string &)> &say = nullptr);

// ---- the items -------------------------------------------------------------------------------------
// One setting: how it is read and written, what Signum sets, what else is fine as it is.
// Signum's transmit drive on the station IC-7300s (both radios, measured on the air 2026-11-19; docs/md/radio-ic7300.md
// "Transmit level"): the USB MOD Level the Signum set-up writes (100 %) and the TX gain it puts in the settings file.
constexpr int kUsbModLevel = 255;
constexpr double kTxGain = 4.0;

struct Item {
	const char *key;            // in the backup file
	const char *name;           // as the radio's menu calls it
	Bytes cmd;                  // read: cmd; write: cmd + data
	Bytes wcmd;                 // the write command when it differs (mode: read 04, write 06)
	Bytes target;               // Signum's value; empty: recorded only, never written
	std::vector<Bytes> fine;    // other values left as they are
	const char *(*show)(const Bytes &, char *, size_t);
};

enum class Profile { SIGNUM, EXTERNAL };

namespace show {
inline const char *raw(const Bytes &b, char *o, size_t n) { std::snprintf(o, n, "%s", hex(b).c_str()); return o; }
inline const char *onoff(const Bytes &b, char *o, size_t n) { std::snprintf(o, n, "%s", b.size() == 1 && b[0] <= 1 ? (b[0] ? "ON" : "OFF") : hex(b).c_str()); return o; }
inline const char *pct(const Bytes &b, char *o, size_t n)
{
	const int v = b.size() == 2 ? bcd(b) : -1;
	if (v < 0 || v > 255) return raw(b, o, n);
	std::snprintf(o, n, "%d%% (%d/255)", int(std::lround(v * 100.0 / 255.0)), v);
	return o;
}
inline const char *pick(const Bytes &b, char *o, size_t n, std::initializer_list<const char *> names)
{
	const size_t i = b.size() == 1 ? b[0] : 99;
	if (i < names.size()) std::snprintf(o, n, "%s", names.begin()[i]); else raw(b, o, n);
	return o;
}
inline const char *afif(const Bytes &b, char *o, size_t n) { return pick(b, o, n, {"AF", "IF"}); }
inline const char *sql(const Bytes &b, char *o, size_t n) { return pick(b, o, n, {"OFF (OPEN)", "ON"}); }
inline const char *modsrc(const Bytes &b, char *o, size_t n) { return pick(b, o, n, {"MIC", "ACC", "MIC,ACC", "USB"}); }
inline const char *usbport(const Bytes &b, char *o, size_t n) { return pick(b, o, n, {"Link to [REMOTE]", "Unlink from [REMOTE]"}); }
inline const char *serfunc(const Bytes &b, char *o, size_t n) { return pick(b, o, n, {"CI-V", "RTTY Decode"}); }
inline const char *keyline(const Bytes &b, char *o, size_t n) { return pick(b, o, n, {"OFF", "DTR", "RTS"}); }
inline const char *agc(const Bytes &b, char *o, size_t n) { return pick(b, o, n, {"OFF", "FAST", "MID", "SLOW"}); }
inline const char *split(const Bytes &b, char *o, size_t n)
{
	if (b.size() == 1 && b[0] <= 1) std::snprintf(o, n, "%s", b[0] ? "ON" : "OFF"); else raw(b, o, n);
	return o;
}
inline const char *mode(const Bytes &b, char *o, size_t n)
{
	static const char *const m[] = {"LSB", "USB", "AM", "CW", "RTTY", "FM", "", "CW-R", "RTTY-R"};
	if (b.size() == 2 && b[0] <= 8 && *m[b[0]] && b[1] >= 1 && b[1] <= 3) std::snprintf(o, n, "%s FIL%d", m[b[0]], b[1]); else raw(b, o, n);
	return o;
}
inline const char *data(const Bytes &b, char *o, size_t n)
{
	if (b.size() == 2 && b[0] <= 1) std::snprintf(o, n, b[0] ? "DATA ON, FIL%d" : "DATA OFF", b[1]); else raw(b, o, n);
	return o;
}
// IF filter width index (1A 03, SSB and the data modes): 00-09 = 50-500 Hz in 50 Hz steps, 10-40 = 600-3600 Hz in 100 Hz steps
inline const char *width(const Bytes &b, char *o, size_t n)
{
	const int i = b.size() == 1 ? bcd(b) : -1;
	if (i < 0 || i > 40) return raw(b, o, n);
	std::snprintf(o, n, "%d Hz", i < 10 ? 50 * (i + 1) : 600 + 100 * (i - 10));
	return o;
}
inline const char *level(const Bytes &b, char *o, size_t n)
{
	const int v = b.size() == 2 ? bcd(b) : -1;
	if (v < 0) return raw(b, o, n);
	std::snprintf(o, n, "%d/255", v);
	return o;
}
inline const char *freq(const Bytes &b, char *o, size_t n)   // 5 bytes little-endian BCD, Hz
{
	if (b.size() != 5) return raw(b, o, n);
	long long f = 0;
	for (int i = 4; i >= 0; i--) f = f * 100 + (b[size_t(i)] >> 4) * 10 + (b[size_t(i)] & 15);
	std::snprintf(o, n, "%.3f kHz", f / 1000.0);
	return o;
}
inline const char *addr(const Bytes &b, char *o, size_t n) { if (b.size() == 1) std::snprintf(o, n, "%02Xh", b[0]); else raw(b, o, n); return o; }
inline const char *addr2(const Bytes &b, char *o, size_t n)   // 0000-0223 = 00h-DFh
{
	const int v = b.size() == 2 ? bcd(b) : -1;
	if (v < 0 || v > 223) return raw(b, o, n);
	std::snprintf(o, n, "%02Xh", v);
	return o;
}
inline const char *rttybaud(const Bytes &b, char *o, size_t n) { return pick(b, o, n, {"4800", "9600", "19200", "38400"}); }
inline const char *preamp(const Bytes &b, char *o, size_t n) { return pick(b, o, n, {"OFF", "P.AMP1", "P.AMP2"}); }
inline const char *att(const Bytes &b, char *o, size_t n) { if (b.size() == 1 && (b[0] == 0 || b[0] == 0x20)) std::snprintf(o, n, "%s", b[0] ? "20 dB" : "OFF"); else raw(b, o, n); return o; }
inline const char *tbw(const Bytes &b, char *o, size_t n) { return pick(b, o, n, {"WIDE", "MID", "NAR"}); }
}  // namespace show

inline Bytes menu(int item) { return {0x1A, 0x05, uint8_t((item / 1000 % 10) << 4 | (item / 100 % 10)), uint8_t((item / 10 % 10) << 4 | (item % 10))}; }

// Every item read. The item numbers and values: Icom's IC-7300 manual ("Control command", 1A 05), checked
// by write and read-back on the bench radios (docs/md/radio-ic7300.md). The order of the writes in setup():
// first what could key the radio by a control line is switched off, the Connectors items, then the
// operating settings (the mode before DATA, both before the filter width), and USB SEND last (it is what
// lets RTS key the transmitter). Never written: RF power, the frequency, and CI-V USB Port (read only
// over CI-V).
const std::vector<Item> &items();
inline const Item *item(const std::string &key)
{
	for (const auto &i : items()) if (key == i.key) return &i;
	return nullptr;
}
inline std::string shown(const Item &it, const Bytes &v) { char b[96]; return it.show(v, b, sizeof b); }
// Recorded only by every profile (power, frequency, ...)? ACC MOD Level has no Signum value but the external set-up writes it.
inline bool never_written(const Item &it) { return it.target.empty() && std::strcmp(it.key, "menu.0064") != 0; }
// What a profile writes to an item: empty = not written (recorded only, or left as it is). The external-modem
// profile (a hardware modem on the ACC socket, instead of Signum on USB) writes the same items as Signum's, so
// that either set-up brings the radio back from the other; four differ: DATA MOD is ACC (the modem's audio comes
// in on the ACC socket), USB SEND is OFF (nothing on USB may key the radio the modem owns), USB MOD Level is
// left as it is (not used with ACC), and ACC MOD Level is written 50 % (128/255, 01 28: the known-good value with
// a hardware modem on the ACC socket). Signum's set-up does not write ACC MOD Level (docs/md/radio-ic7300.md).
Bytes wanted(const Item &it, Profile pr);

// One item as read, and what set-up does with it.
struct Row {
	const Item *it = nullptr;
	bool read = false;          // the radio answered the read
	Bytes now;
	bool change = false;        // set-up writes it
	Bytes want;                 // what the profile sets (empty: not written)
	std::string now_s, new_s;
};

struct Plan {
	Radio radio;
	Probe probe;
	Profile profile = Profile::SIGNUM;
	std::vector<Row> rows;
	bool tx = false;            // the radio was transmitting (1C 00 = 01): nothing is written
	bool tx_read = false;
	int changes() const { int n = 0; for (const auto &r : rows) n += r.change; return n; }
	const Row *row(const std::string &key) const { for (const auto &r : rows) if (key == r.it->key) return &r; return nullptr; }
};

using Say = std::function<void(const std::string &stage, const std::string &text, int ok)>;   // ok: 1 good, 0 bad, -1 information

void read_all(Link &l, Plan &p);

// ---- the backup file -------------------------------------------------------------------------------
inline std::string config_dir()
{
	const char *x = std::getenv("XDG_CONFIG_HOME"), *h = std::getenv("HOME");
	return x && *x ? std::string(x) + "/signum" : h ? std::string(h) + "/.config/signum" : std::string();
}
inline std::string utc_stamp(bool file)
{
	const time_t t = ::time(nullptr);
	tm g{};
	gmtime_r(&t, &g);
	char b[32];
	std::strftime(b, sizeof b, file ? "%Y%m%dT%H%M%SZ" : "%Y-%m-%d %H:%M:%S UTC", &g);
	return b;
}
// A new backup file's name: ic7300-backup-SERIAL-TIME.conf; a second one within the same second gets a letter after
// the time (b, c, ...), which sorts as newer in backups().
inline std::string backup_path(const std::string &dir, const std::string &serial)
{
	const std::string base = dir + "/ic7300-backup-" + serial + "-" + utc_stamp(true);
	std::string p = base + ".conf";
	struct stat st{};
	for (char c = 'b'; c <= 'z' && ::stat(p.c_str(), &st) == 0; c++) p = base + c + ".conf";
	return p;
}
// "" or why not. Written 0600, fsync'd, before anything is changed. rf_power: the backup of --ic7300-power
// (ic7300_power.hpp): marked so that its restore writes RF POWER back too (no other backup's restore does).
std::string write_backup(const Plan &p, const std::string &path, const std::string &why, bool rf_power = false);
struct Backup {
	std::string serial, port;
	uint8_t addr = 0x94;
	int baud = 0;
	std::vector<std::pair<std::string, Bytes>> values;   // writable items only, in the file's order
	std::vector<std::string> warnings;
	bool rf_power = false;      // a backup of --ic7300-power ("restores_rf_power = yes"): RF POWER is among the values
};
bool read_backup(const std::string &path, Backup &b, std::string &err);
std::vector<std::string> backups();

// ---- writing --------------------------------------------------------------------------------------
struct Result {
	bool ok = false;
	int written = 0, verified = 0, failed = 0;
	std::string backup, error;
};

// One item: written, then read back and compared (one more try on a mismatch).
bool put(Link &l, const Item &it, const Bytes &v, std::string &why);

// Write a list of (item, value) in items() order. The three control-line items (USB SEND, USB Keying CW and
// RTTY) may not share a line (the radio refuses it): each that changes from a line goes to OFF first, then
// the other items, then CW and RTTY, and USB SEND last.
void write_list(Link &l, Plan &plan, const std::vector<std::pair<const Item *, Bytes>> &w, Result &res, const Say &say, const char *stage);

// Detect and read one radio (no writes): the plan. err: why not.
bool examine(util::SafePort &p, const Radio &r, Plan &plan, bool allow_b0_exit, std::string &err, const Say &say, Profile profile = Profile::SIGNUM);

// The set-up proper, on an examined radio (examine() just before, the port still open): backup, writes.
Result setup(util::SafePort &p, Plan &plan, const Say &say);

// A backup file written back to an examined radio (the same serial number).
Result restore(util::SafePort &p, Plan &plan, const Backup &b, const Say &say);

// What Signum's settings file gets for this radio (bench-measured values; docs/md/radio-ic7300.md).
std::vector<std::pair<std::string, std::string>> signum_settings(const Radio &r);

// ---- the warning -----------------------------------------------------------------------------------
std::string warning_text(const Plan &p, const std::string &backup_dir);

}  // namespace ic7300
}  // namespace radio

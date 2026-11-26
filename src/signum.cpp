// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// signum: our P1/P2/P3 modem (src/modem/, checked against the reference unit) behind
// the classic host-mode interface on a pty. See README.md.
//
//   signum --call MYCALL --alsa DEV [--ptt SPEC] [--pty PATH] [options]
//   signum --call MYCALL --audio-in FIFO --audio-out FIFO [--pace] [...]
//
// Audio: ALSA at 48 kHz mono, resampled 5:1 to the modem's 9600 Hz (the
// capture read paces the modem in real time; the real-time audio
// path: prime, playback re-anchor, self-timing, audio_ppm, self-calibration), or the emulator's
// lockstep FIFO protocol (per 9600 Hz sample: write ours, read the peer's)
// for tests, two instances, or the emulated reference unit (its
// --audio-in/--audio-out).
// PTT: src/radio/ptt.hpp (RTS, DTR, CI-V, rigctld, VOX, none), keyed
// while the modem transmits, released after the queued audio has played;
// key-down watchdog --max-key.
#include <alsa/asoundlib.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core_impl.hpp"
#include "start_config.hpp"
#include "ic7300_cli.hpp"
#include "json_mini.hpp"
#include "telnet.hpp"
#include "async_out.hpp"
#include "audio_rec.hpp"
#include "ptt.hpp"
#include "rig_tune.hpp"
#include "rig_alc.hpp"
#include "cpu_clock.hpp"
#include "pty_excl.hpp"
#include "signum/resample5.hpp"
#include "signum/clock_skew.hpp"
#include "signum/autocal.hpp"
#if defined(SIGNUM_CRYPTO) && !defined(SIGNUM_NO_CRYPTO)
#include "crypto/crypto_cli.hpp"
#endif

#ifndef SIGNUM_VERSION
#define SIGNUM_VERSION "dev"
#endif

static const snd_pcm_uframes_t kPeriod = 24;            // 2.5 ms at 9600 Hz
static const long kPrime = 384;                         // 40 ms at 9600 Hz
static radio::Ptt g_ptt;
static signum::CpuClock g_cpu;
static AsyncOut g_out;
static int g_so = -1;
static volatile std::sig_atomic_t g_stop = 0;
static double mono_s() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static pid_t g_ppid0 = 0;                           // the parent at start (a different one later: it exited)
static volatile double g_stop_t = 0;
static double mono_sig()                             // (async-signal-safe)
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return double(ts.tv_sec) + double(ts.tv_nsec) * 1e-9;
}
// A stop signal is a normal stop: exit status 0. The same stop sent again within a second is
// ignored: GNU timeout (and other supervisors) signal the child and then its whole process group,
// and that second SIGTERM used to end it at once with status 1 (the bench's "station rc 1").
// Asked again later: out at once, still 0. The stop itself is given 3 s (on_alarm).
static void on_stop_signal(int sig)
{
	g_ptt.force_off();
	const double now = mono_sig();
	if (g_stop) {
		if (now - g_stop_t < 1.0) return;
		g_ptt.rest_now();
		g_cpu.restore();
		_exit(0);
	}
	g_stop_t = now;
	g_stop = sig;
	alarm(3);
}
// The line logged on the way out after a stop signal (a station started from a script that
// exits gets SIGTERM from PR_SET_PDEATHSIG and used to vanish without a word).
static std::string stop_text(bool can_key)
{
	const int s = g_stop;
	std::string t = std::string("[signum] ") + (s == SIGINT ? "SIGINT" : s == SIGHUP ? "SIGHUP" : "SIGTERM");
	if (getppid() != g_ppid0) t += " (the parent process exited" + std::string(can_key ? ": a station that can key ends with its parent, docs/md/running.md \"In the background\")" : ")");
	return t + (can_key ? ": unkeyed, exiting\n" : ": exiting\n");
}
static void on_alarm(int)                            // the stop took over 3 s: an error (status 1)
{
	g_ptt.rest_now();
	g_cpu.restore();
	static const char m[] = "[signum] the stop took over 3 s: unkeyed, forced exit\n";
	(void)!write(2, m, sizeof m - 1);
	_exit(1);
}
static void unkey_exit() { g_ptt.force_off(); }
// A crash (SIGSEGV, SIGABRT, ...): unkeyed (CI-V "TX off" too), the port's lines low and at B0, then the
// default action (the core dump). SIGKILL cannot be caught: HUPCL lowers the lines at the port's last
// close (src/util/serial_safe.hpp), and the radio's own timer is all that ends a CI-V key.
static void on_crash(int sig)
{
	g_ptt.rest_now();
	g_cpu.restore();
	static const char m[] = "[signum] crashed: unkeyed\n";
	(void)!write(2, m, sizeof m - 1);
	signal(sig, SIG_DFL);
	raise(sig);
}
static void cpu_exit() { g_cpu.restore(); }

// ---- the pty: client 0 of the core ----------------------------------------------
struct Pty {
	int fd = -1, slave = -1;
	std::string link, pend, slave_path;
	std::chrono::steady_clock::time_point excl_check{};
	bool open_at(const std::string &path)
	{
		fd = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
		if (fd < 0 || grantpt(fd) || unlockpt(fd)) { std::perror("pty"); return false; }
		const char *sl = ptsname(fd);
		// Hold the slave open ourselves so the master never sees EIO between
		// client sessions; raw, no echo (a client sets its own mode anyway).
		slave_path = sl;
		slave = ::open(sl, O_RDWR | O_NOCTTY | O_CLOEXEC);
		termios t; tcgetattr(slave, &t); cfmakeraw(&t); tcsetattr(slave, TCSANOW, &t);
		struct stat lst;
		if (::lstat(path.c_str(), &lst) == 0 && !S_ISLNK(lst.st_mode)) {   // only our own link from an earlier run is replaced
			std::fprintf(stderr, "[pty] %s exists and is not a link: not replaced (--pty PATH)\n", path.c_str());
			return false;
		}
		::unlink(path.c_str());
		if (::symlink(sl, path.c_str())) { std::perror(path.c_str()); return false; }
		link = path;
		std::printf("[pty] host-mode serial interface on %s -> %s\n", path.c_str(), sl);
		return true;
	}
	~Pty() { if (!link.empty()) ::unlink(link.c_str()); }
	void pump(signum::Core &core)
	{
		uint8_t b[1024];
		for (ssize_t r; (r = ::read(fd, b, sizeof b)) > 0;) core.input(0, b, size_t(r));
		pend += core.take_output(0);
		// A client that left with the exclusive flag set (killed, or an error exit without TIOCNXCL) would lock
		// the next one out with EBUSY: clear it through our own slave descriptor once nobody else has it open.
		const auto now = std::chrono::steady_clock::now();
		if (slave >= 0 && now - excl_check >= std::chrono::seconds(1)) {
			excl_check = now;
			if (signum::pty_clear_leftover_excl(slave, slave_path))
				std::printf("[pty] cleared the exclusive flag a client left on %s\n", slave_path.c_str());
		}
		if (pend.empty()) return;
		const ssize_t w = ::write(fd, pend.data(), pend.size());
		if (w > 0) pend.erase(0, size_t(w));
		if (pend.size() > (1u << 20)) pend.erase(0, pend.size() - (1u << 20));   // nobody reads: keep the newest MB
	}
};

// ---- calibration  ---------
static float g_hw_latency = -1;
static long g_key_delay = 0;                           // fix 243: samples (9600 Hz) the PTT keys after our audio starts
static bool g_file_timing = false, g_file_ppm = false;
static double g_audio_ppm = 0, g_skew_s = 0;
static constexpr float kRtFromRound = 0.065f, kRaFromRound = 0.068f;
// Self-calibration (src/modem/signum/autocal.hpp) when the file gives no hw_latency_s / audio_ppm:
//  - hardware round trip: until readings confirm one, kHwGuess [a guess; the bench's sum below is
//    22 ms, and called-station links on the bench received in full with 5 ms, 2026-11-13]. A reading is the callee's P1 answer
//    to our call less what we know of the loop: our software round trip, the rate-correction filter,
//    and kPeerFrame. kPeerFrame = 0 [measured]: against the reference unit on the bench (2026-11-13,
//    four answers in two links) answer - 0.995833 s - software round trip - filter came to 21.5-22.1 ms,
//    the same as the bench's working hw_latency_s 0.022, which was derived from the reference unit's
//    answer in the same way. So "hardware" here is our codec + radio plus the reference unit's own
//    audio path, as the calibration file has always held it, and the timing rules (kRtFromRound,
//    kRaFromRound) were set on that sum. How it splits between the two ends is not measured.
//    (An earlier 68.2 ms, inferred from our own callee rule, would give readings ~46 ms below zero,
//    outside AutoCal's range: no reading against the reference unit at all.)
//  - the sound card's rate error: from our own transmissions as the link's slave (autocal.hpp).
// Both apply between links and are kept in the state file (per sound card).
// Fix 262 (on the air 2026-11-18, TODO ff 3): kHwGuess was 5 ms. A station that is only ever called
// never reads its hardware round trip (the readings come from answers to OUR calls), so it kept the
// guess for good, and its round trip came out ~15 ms short of a typical radio's (the bench's 18.8-22.1 ms
// against the reference unit): rt_latency 15 ms too small, every callee answer 15 ms late. With two
// Signums on the air the calibrated caller read the never-calibrated callee's call answers at +1.120-
// 1.123 s after its first bit (a calibrated callee's at +1.101-1.106 s, both its calls linked at once):
// too late for its decision at 1.22 s (10 of the answer's 12 bits by then), so of 11 calls 5 linked one
// cycle late (the answer read whole after the decision) and 6 never ("no answer"). Now the guess is the typical radio's
// value, so a never-calibrated callee answers as a calibrated one does. SIGNUM_HW_GUESS_OLD=1: 5 ms.
// (A Signum callee still answers ~40 ms after the reference unit on the air: the unit answers 35 ms
// after a P1 packet's end, less than our software round trip alone.)
static const float kHwGuess = std::getenv("SIGNUM_HW_GUESS_OLD") ? 0.005f : 0.020f;
static constexpr double kPeerFrame = 0.0;
// The sound card against the system's monotonic clock, for the very first link (no reading from a
// far station yet; the reference unit's crystal and an NTP-disciplined clock agree within a few ppm,
// the bench's codec read -137 ppm against true time while the reference unit read 130 ppm short
// on it). Per 10 s window the smallest (clock - samples/9600): a capture read is only ever late, so
// the minima lie on the true line. Fit after >= 30 windows without an xrun; used only when both
// halves agree within 5 ppm, and only until a far station gave a reading. Without NTP the clock
// is the computer's own crystal (+-50 ppm), hence the far station's reading always replaces it.
struct MonoRate {
	static constexpr int kWin = 64;
	double wt[kWin] = {}, wo[kWin] = {};
	int nw = 0;
	double cur_min = 1e30, cur_t = 0;
	uint64_t win_start = 0;
	bool started = false;
	void reset() { nw = 0; started = false; cur_min = 1e30; }
	void add(uint64_t n, double mono)
	{
		const double t = double(n) / 9600.0, off = mono - t;
		if (!started) { started = true; win_start = n; cur_min = 1e30; }
		if (off < cur_min) { cur_min = off; cur_t = t; }
		if (n - win_start >= 96000) {                 // 10 s
			if (nw == kWin) { std::memmove(wt, wt + 1, sizeof(double) * (kWin - 1)); std::memmove(wo, wo + 1, sizeof(double) * (kWin - 1)); nw--; }
			wt[nw] = cur_t; wo[nw] = cur_min; nw++;
			win_start = n; cur_min = 1e30;
		}
	}
	static double slope(const double *t, const double *o, int m)
	{
		double st = 0, so = 0, stt = 0, sto = 0;
		for (int i = 0; i < m; i++) { st += t[i]; so += o[i]; stt += t[i] * t[i]; sto += t[i] * o[i]; }
		const double d = m * stt - st * st;
		return d > 0 ? (m * sto - st * so) / d : 0.0;
	}
	// The card's rate error (ppm, + = fast), when trustworthy.
	bool estimate(double &ppm, double &h1, double &h2) const
	{
		if (nw < 30) return false;
		const int h = nw / 2;
		ppm = -slope(wt, wo, nw) * 1e6; h1 = -slope(wt, wo, h) * 1e6; h2 = -slope(wt + h, wo + h, nw - h) * 1e6;
		return std::fabs(h1 - h2) <= 5.0;
	}
};

static std::string default_autocal_path(const std::string &dev)
{
	const char *x = std::getenv("XDG_CONFIG_HOME"), *h = std::getenv("HOME");
	std::string d = dev;
	for (auto &c : d) if (!std::isalnum(uint8_t(c))) c = '_';
	return x && *x ? std::string(x) + "/signum/autocal-" + d + ".conf" : h ? std::string(h) + "/.config/signum/autocal-" + d + ".conf" : "";
}
static std::string default_cal_path()
{
	const char *x = std::getenv("XDG_CONFIG_HOME"), *h = std::getenv("HOME");
	return x && *x ? std::string(x) + "/signum/calibration.conf" : h ? std::string(h) + "/.config/signum/calibration.conf" : "";
}
static bool load_cal(const std::string &path, fmodem::P2Config &cfg, bool timing)
{
	FILE *f = path.empty() ? nullptr : std::fopen(path.c_str(), "r");
	if (!f) return false;
	char line[256];
	while (std::fgets(line, sizeof line, f)) {
		char key[64]; double v;
		if (line[0] == '#' || std::sscanf(line, " %63[a-z_] = %lf", key, &v) != 2) continue;
		if (timing && !std::strcmp(key, "rt_latency_s")) { cfg.rt_latency_s = float(v); g_file_timing = true; }
		else if (timing && !std::strcmp(key, "reply_adjust_s")) { cfg.reply_adjust_s = float(v); g_file_timing = true; }
		else if (timing && !std::strcmp(key, "hw_latency_s")) { g_hw_latency = float(v); g_file_timing = true; }
		else if (!std::strcmp(key, "audio_ppm")) { g_audio_ppm = v; g_file_ppm = true; }
	}
	std::fclose(f);
	return true;
}

// One open + configure attempt; 0 = ok, else the (negative) errno of the step that failed.
static int open_pcm_once(snd_pcm_t **hp, const char *dev, snd_pcm_stream_t dir, snd_pcm_uframes_t buffer, bool *opened)
{
	snd_pcm_t *h;
	*opened = false;
	int e = snd_pcm_open(&h, dev, dir, 0);
	if (e < 0) return e;
	*opened = true;
	snd_pcm_hw_params_t *hw;
	snd_pcm_hw_params_alloca(&hw);
	snd_pcm_hw_params_any(h, hw);
	unsigned rate = 48000;
	snd_pcm_uframes_t period = kPeriod * 5;
	int d = 0;
	if ((e = snd_pcm_hw_params_set_access(h, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
	    (e = snd_pcm_hw_params_set_format(h, hw, SND_PCM_FORMAT_S16_LE)) < 0 ||
	    (e = snd_pcm_hw_params_set_channels(h, hw, 1)) < 0 ||
	    (e = snd_pcm_hw_params_set_rate_near(h, hw, &rate, &d)) < 0 ||
	    (e = snd_pcm_hw_params_set_period_size_near(h, hw, &period, &d)) < 0 ||
	    (e = snd_pcm_hw_params_set_buffer_size_near(h, hw, &buffer)) < 0 ||
	    (e = snd_pcm_hw_params(h, hw)) < 0 || rate != 48000) {
		if (e >= 0) e = -EINVAL;
		snd_pcm_close(h);
		return e;
	}
	*hp = h;
	return 0;
}

// A sound server (PipeWire probing a snd-aloop card) can hold a subdevice for a moment as a test
// station starts: EBUSY is retried a few times (bounded, ~1 s, each retry logged); a card that is
// genuinely taken still fails clearly after that, and every other error fails at once.
static snd_pcm_t *open_pcm(const char *dev, snd_pcm_stream_t dir, snd_pcm_uframes_t buffer)
{
	snd_pcm_t *h = nullptr;
	const int kTries = 6;
	for (int i = 1;; i++) {
		bool opened;
		int e = open_pcm_once(&h, dev, dir, buffer, &opened);
		if (e == 0) return h;
		if (e == -EBUSY && i < kTries) {
			std::fprintf(stderr, "%s busy (%s), retry %d/%d in 150 ms\n", dev, snd_strerror(e), i, kTries - 1);
			usleep(150000);
			continue;
		}
		if (!opened) std::fprintf(stderr, "cannot open %s (%s)\n", dev, snd_strerror(e));
		else std::fprintf(stderr, "cannot configure %s (48 kHz mono S16): %s\n", dev, snd_strerror(e));
		std::exit(1);
	}
}

static void usage(FILE *f)
{
	std::fprintf(f,
	    "signum %s: Signum, a P1/P2/P3 HF data modem on a pty\n\n"
	    "  --call CALL            MYcall (default N0CALL)\n"
	    "  --pty PATH             the host-mode serial interface (default /tmp/ttysignum0; 'none': no pty)\n"
	    "  --alsa DEV             sound card (e.g. plughw:CARD=CODEC), real time, 48 kHz mono\n"
	    "  --audio-in F --audio-out F   instead: the lockstep FIFO link (tests, a second instance, the emulator)\n"
	    "  --pace                 FIFO link at real time (else as fast as the peer)\n"
	    "  --duration S           FIFO link: stop after S s of audio (default: run until stopped)\n"
	    "  --ptt SPEC             rts:TTY | dtr:TTY | civ:TTY:ADDR[:BAUD] | rig[:HOST[:PORT]] | vox | none\n"
	    "                         (civ: ADDR, the radio's CI-V address in hex, is required: IC-7300 94)\n"
	    "                         (vox cannot meet the P1/P2/P3 turnaround: the far station's answers are lost)\n"
	    "                         (rig: also reads the frequency for the panel: read only, in standby only)\n"
	    "  --rts TTY              = --ptt rts:TTY\n"
	    "  --max-key S            key-down watchdog (default 90 s)\n"
	    "  --tx-gain DB           output level on top of PSKAmpl (default 0)\n"
	    "  --tones LO,HI          tone pair (default 1400,1600: TONES 4)\n"
	    "  --mylevel 1|2|3        highest level (default 3)\n"
	    "  --alc-warn on|off      warn when the radio's ALC acts while we send P2/P3 (ALCwarn): on (default) reads the\n"
	    "                         ALC meter through rigctld (--ptt rig:, `l ALC`) while keyed on a P2/P3 link and\n"
	    "                         logs [alc] once per link when the drive is squashing the PSK peaks, with a summary at\n"
	    "                         the link's end. Warns only: never changes TX gain or power. off: no ALC reading\n"
	    "  --tune-tone S          tune before each new call we place, for S s (1-10; default 0 = off): the radio's own\n"
	    "                         tuner cycle through rigctld, or a steady tone for an automatic antenna tuner\n"
	    "                         (TUnetone); not on call repeats or as callee\n"
	    "  --tune-method M        auto (default): the radio's tune when --ptt rig: reaches a rigctld whose rig lists the\n"
	    "                         TUNE op, else the tone; tone: always the tone; radio: the radio's tune, the tone (with\n"
	    "                         the reason in the log) when that is not possible. The radio transmits at ITS RF POWER\n"
	    "  --tune-hz HZ           its frequency (300-2700, default 1500)\n"
	    "  --tune-db DB           its level against the data level (-30-0, default 0 = the P1 call's power)\n"
	    "  --pduplex on|off       automatic changeover (PDuplex), every mode: off (default, as the reference unit)\n"
	    "                         breaks in as receiver only on the sender's changeover, the break-in key (Ctrl-Y,\n"
	    "                         host %%O/%%I) or our QRT, and hands over as sender only on Ctrl-Y (host %%O);\n"
	    "                         on also breaks in whenever text is waiting (not within --pdtimer s of the other's\n"
	    "                         break-in) and hands over whenever our text has run out\n"
	    "  --pdtimer S            PDTimer, 2-30 s (default 12)\n"
	    "  --listen [on|off]      listen mode at start (Listen): on (default, as the reference unit) decodes and\n"
	    "                         shows other stations' links while idle; a bare --listen is on\n"
	    "  --p3-unproto-rx        Listen mode also prints P3 unproto (U 30-41; the reference unit cannot)\n"
	    "  --peer-marker on|off   the Signum peer marker (PEERmark): on (default; owner's choice, the reference unit\n"
	    "                         has none) sends a short system sequence on each P2/P3 link that the reference unit\n"
	    "                         skips silently, so two Signums know each other (the panel's Signum / Other lamps);\n"
	    "                         off sends nothing extra. Never on P1\n"
#if defined(SIGNUM_CRYPTO) && !defined(SIGNUM_NO_CRYPTO)
	    "                         (with encryption or verify on, that layer's record is the marker)\n"
#endif

	    "  --cal FILE             timing calibration (default ~/.config/signum/calibration.conf)\n"
	    "  --rt-latency S         instead of the file's timing\n"
	    "  --audio-ppm E          the sound card's rate error (else env AUDIO_PPM, else the file)\n"
	    "  --no-autocal           no self-calibration (it measures what the file and options do not give)\n"
	    "  --autocal-state FILE   where it keeps what it measured (default ~/.config/signum/autocal-DEV.conf)\n"
	    "  --autocal-max-ms N     self-calibration: hardware round-trip readings above N ms are refused (default 45;\n"
	    "                         env AUTOCAL_HW_MAX_MS=N the same, this option wins). Raise it for a radio path\n"
	    "                         known to be slower (an SDR transceiver); a far station on this modem code\n"
	    "                         reads ~60 ms and is then taken for hardware\n"
	    "  --cpu-adaptive on|off  adaptive CPU features at start (CPUadapt): on (default) uses long packets as P3\n"
	    "                         receiver and drops them by level when this computer's decodes come late; off\n"
	    "                         never asks for them (env P3_LONG_RT=0/1 forces never/always, for tests)\n"
	    "  --prime-ms MS          playback prime (default 40)\n"
	    "  --cpu-clock MODE       with --alsa: auto (the CPU clock at its maximum while a link is up), max (while\n"
	    "                         running), off (left to the governor); default auto. Needs a writable\n"
	    "                         scaling_min_freq (docs/md/performance.md \"CPU clock\")\n"
	    "  --rx-noise RMS         test: noise added to what we hear (FIFO link)\n"
	    "  --quiet                no modem log on stdout\n"
	    "  --telnet PORT          the command interpreter on raw TCP/telnet (default %d; 0 = off)\n"
	    "  --telnet-bind ADDR     its address (default 127.0.0.1; 0.0.0.0 for every interface)\n"
	    "  --telnet-auth PASS     a password prompt before the interpreter attaches (default open)\n"
	    "  --config FILE          the settings file (default ~/.config/signum/signum.conf; env SIGNUM_CONFIG):\n"
	    "                         \"option = value\" lines, read first, so the command line overrides it; the GUI's\n"
	    "                         Configure tab writes it\n"
	    "  --no-config            no settings file (SIGNUM_CONFIG=none the same)\n"
	    "Icom IC-7300 (docs/md/radio-ic7300.md \"Zero-effort set-up\"; one action, then Signum exits):\n"
	    "  --detect-ic7300        find the IC-7300s (port, sound card, CI-V rate and address) and show their\n"
	    "                         settings and what the set-up would change; reads only, changes nothing\n"
	    "  --backup-ic7300-settings   save the radio's settings to such a backup file; changes nothing\n"
	    "  --configure-my-ic7300-and-overwrite-its-settings\n"
	    "                         RECONFIGURE the radio for Signum over CI-V (Connectors menu, USB-D, filter,\n"
	    "                         AGC, NB/NR/notch/COMP/VOX off, USB SEND = RTS), each read back; saves the\n"
	    "                         radio's settings to ~/.config/signum/ic7300-backup-SERIAL-TIME.conf first, then\n"
	    "                         writes alsa, ptt, ... into the settings file. Never changes RF power or frequency.\n"
	    "                         Asks you to type YES\n"
	    "  --configure-my-ic7300-for-an-external-modem-and-overwrite-its-settings\n"
	    "                         the same, but for a hardware modem on the radio's ACC socket instead of Signum on\n"
	    "                         USB: DATA MOD = ACC, USB SEND and USB Keying OFF (nothing on USB can key it), ACC MOD\n"
	    "                         Level 50 percent; the\n"
	    "                         rest as for Signum, so either command brings the radio back from the other. Does\n"
	    "                         not touch Signum's settings file. Never changes RF power or frequency. Asks for YES\n"
	    "  --restore-ic7300-settings FILE   write such a backup back to its radio (asks you to type YES); a backup of\n"
	    "                         --ic7300-power puts RF POWER and the USB MOD Level back too\n"
	    "  --ic7300-power W       set the drive and RF POWER so that our P2/P3 PSK peaks reach W watts with the ALC\n"
	    "                         at rest (docs/md/radio-ic7300.md \"Setting the power\"): the model's TX gain for W, safe on\n"
	    "                         a hot radio (-7 dB for 20 W, -7.5 dB for 25 W), USB MOD\n"
	    "                         Level 100 percent, RF POWER just above the peaks;\n"
	    "                         saves the TX gain in the settings file (old -> new printed). Backup first (restores\n"
	    "                         RF POWER and USB MOD Level); never transmits\n"
	    "  --measure              with --ic7300-power: TRANSMITS a few short steady tones (at most 1.5 s each, 12 s in\n"
	    "                         all; RF POWER is the final one from the start, the TX gain starts low and steps up) to find the TX gain with the ALC at\n"
	    "                         rest, then sets RF POWER and saves the TX gain. A watchdog process unkeys the radio\n"
	    "                         if Signum dies while keyed. Asks for YES; the sound card must be free (Signum not running)\n"
	    "  --ic7300-serial N      which radio, when there is more than one\n"
	    "  --i-understand-this-overwrites-my-ic7300-settings   (scripts) confirm without typing YES\n"
	    "  --version, --help\n", SIGNUM_VERSION, signum::kTelnetDefaultPort);
#if defined(SIGNUM_CRYPTO) && !defined(SIGNUM_NO_CRYPTO)
	signum::crypto::crypto_usage(f);
#endif
	signum::frontend_usage(f);
}

int main(int argc, char **argv)
{
	{	// the IC-7300 set-up, detection and restore: one action, then exit (src/ic7300_cli.hpp)
		bool help = false;
		for (int i = 1; i < argc; i++) help = help || !std::strcmp(argv[i], "--help") || !std::strcmp(argv[i], "-h") || !std::strcmp(argv[i], "--version");
		signum::Ic7300Args ia;
		std::string e;
		if (!help) {
			if (!signum::ic7300_args(argc, argv, ia, e)) { std::fprintf(stderr, "%s\n", e.c_str()); return 2; }
			if (ia.any()) return signum::ic7300_main(ia);
		}
	}
#if defined(SIGNUM_CRYPTO) && !defined(SIGNUM_NO_CRYPTO)
	signum::crypto::CryptoOptions crypto_opt;
#endif
	fmodem::P2Config cfg;
	cfg.amplitude = 4400.0f;                       // the reference firmware's PSK peak level (PSKAmpl 140)
	cfg.allow_p3 = true;
	cfg.inline_status = false;                     // link texts come from the host interface
	cfg.p3_greeting = "\rSignum: P1/P2/P3 station\r";   // the callee's connect text (CMsg 0: none)
	signum::HostParams par;
	std::string pty_path = "/tmp/ttysignum0", alsa, ain, aout, cal_path = default_cal_path();
	radio::PttSpec ptt_spec; bool ptt_given = false;
	double max_key_s = 90, duration = -1, prime_ms = -1, rx_noise = 0, audio_ppm_cli = 0;
	bool pace = false, rtl_given = false, quiet = false, ppm_given = false, no_autocal = false;
	std::string ac_path;
	double autocal_max_ms = -1;
	signum::CpuClock::Mode cpu_mode = signum::CpuClock::AUTO;
	int mylevel = 3;
	int tune_method = signum::TuneTone::AUTO;
	int tune_s = 0, tune_hz = signum::TuneTone::kDefHz, tune_db = signum::TuneTone::kDefDb;
	bool alc_warn = true;                          // --alc-warn (ALCwarn): on by default (owner, 2026-11-23)
	bool peer_marker = true;                       // --peer-marker (PEERmark): on by default (owner, 2026-11-16)
	signum::TelnetServer telnet;
	// The settings file first (src/start_config.hpp), then the command line, which wins.
	signum::StartConfig scfg;
	scfg.path = signum::StartConfig::default_path();
	for (int i = 1; i < argc; i++) {
		const std::string a = argv[i];
		if (a == "--no-config") scfg.path.clear();
		else if (a == "--config" && i + 1 < argc) scfg.path = argv[++i];
		else if (a == "--help" || a == "-h" || a == "--version") { scfg.path.clear(); break; }
	}
	scfg.load();
	for (const auto &w : scfg.warnings) std::fprintf(stderr, "[config] %s\n", w.c_str());
	std::vector<std::string> args_s = scfg.args();
	const size_t n_file_args = args_s.size();
	for (int i = 1; i < argc; i++) args_s.push_back(argv[i]);
	std::vector<char *> av{argv[0]};
	for (auto &x : args_s) av.push_back(&x[0]);
	av.push_back(nullptr);
	argc = int(av.size()) - 1;
	argv = av.data();
	for (int i = 1; i < argc; i++) {
		const std::string a = argv[i];
		if (signum::frontend_arg(i, argc, argv)) continue;
		auto val = [&]() -> const char * { if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", a.c_str()); std::exit(2); } return argv[++i]; };
		if (a == "--no-config") continue;
		if (a == "--config") { val(); continue; }
		if (a == "--call") cfg.mycall = val();
		else if (a == "--pty") pty_path = val();
		else if (a == "--alsa") alsa = val();
		else if (a == "--audio-in") ain = val();
		else if (a == "--audio-out") aout = val();
		else if (a == "--pace") pace = true;
		else if (a == "--duration") duration = std::atof(val());
		else if (a == "--ptt") { if (!ptt_spec.parse(val())) { std::fprintf(stderr, "bad --ptt %s\n", argv[i]); return 2; } ptt_given = true; }
		else if (a == "--rts") { ptt_spec.parse(std::string("rts:") + val()); ptt_given = true; }
		else if (a == "--max-key") max_key_s = std::atof(val());
		else if (a == "--tx-gain") par.tx_gain_db = float(std::atof(val()));
		else if (a == "--tones") { if (std::sscanf(val(), "%f,%f", &cfg.tone_lo, &cfg.tone_hi) != 2) { std::fprintf(stderr, "bad --tones\n"); return 2; } }
		else if (a == "--mylevel") mylevel = std::max(1, std::min(3, std::atoi(val())));
		else if (a == "--listen") { if (i + 1 < argc && (!std::strcmp(argv[i + 1], "on") || !std::strcmp(argv[i + 1], "off"))) par.listen = !std::strcmp(argv[++i], "on"); else par.listen = 1; }
		else if (a == "--peer-marker") { const std::string v = val(); if (v != "on" && v != "off") { std::fprintf(stderr, "bad --peer-marker %s (on or off)\n", v.c_str()); return 2; } peer_marker = v == "on"; }
		else if (a == "--alc-warn") { const std::string v = val(); if (v != "on" && v != "off") { std::fprintf(stderr, "bad --alc-warn %s (on or off)\n", v.c_str()); return 2; } alc_warn = v == "on"; }
		else if (a == "--tune-tone" || a == "--tune-hz" || a == "--tune-db") {
			const char *v = val(); char *e = nullptr;
			const long x = std::strtol(v, &e, 10);
			const int k = int(std::max(-100000L, std::min(100000L, x)));
			const bool ok = e != v && !*e && (a == "--tune-tone" ? signum::TuneTone::valid_secs(k) : a == "--tune-hz" ? signum::TuneTone::valid_hz(k) : signum::TuneTone::valid_db(k));
			if (!ok) { std::fprintf(stderr, "bad %s %s (%s)\n", a.c_str(), v, a == "--tune-tone" ? "0-10 s" : a == "--tune-hz" ? "300-2700 Hz" : "-30-0 dB"); return 2; }
			(a == "--tune-tone" ? tune_s : a == "--tune-hz" ? tune_hz : tune_db) = k;
		}
		else if (a == "--tune-method") { if (!signum::TuneTone::parse_method(val(), tune_method)) { std::fprintf(stderr, "bad --tune-method (auto, tone or radio)\n"); return 2; } }
		else if (a == "--p3-unproto-rx") setenv("P3U_RX", "1", 1);   // P3 unproto (U 30-41) also printed in Listen mode: ours, not the reference unit's
		else if (a == "--cal") cal_path = val();
		else if (a == "--rt-latency") { cfg.rt_latency_s = float(std::atof(val())); rtl_given = true; }
		else if (a == "--audio-ppm") { audio_ppm_cli = std::atof(val()); ppm_given = true; }
		else if (a == "--prime-ms") prime_ms = std::atof(val());
		else if (a == "--no-autocal") no_autocal = true;
		else if (a == "--cpu-clock") { if (!signum::CpuClock::parse(val(), cpu_mode)) { std::fprintf(stderr, "bad --cpu-clock %s (auto, max or off)\n", argv[i]); return 2; } }
		else if (a == "--autocal-state") ac_path = val();
		else if (a == "--autocal-max-ms") { autocal_max_ms = std::atof(val()); if (!(autocal_max_ms > 0 && autocal_max_ms <= 1000)) { std::fprintf(stderr, "bad --autocal-max-ms (0 < N <= 1000)\n"); return 2; } }
		else if (a == "--pduplex") { const std::string v = val(); if (v != "on" && v != "off") { std::fprintf(stderr, "bad --pduplex %s (on or off)\n", v.c_str()); return 2; } cfg.pduplex = v == "on"; }
		else if (a == "--pdtimer") { const char *v = val(); char *e = nullptr; const long k = std::strtol(v, &e, 10); if (e == v || *e || k < 2 || k > 30) { std::fprintf(stderr, "bad --pdtimer %s (2-30 s)\n", v); return 2; } cfg.pdtimer_s = int(k); }
		else if (a == "--cpu-adaptive") { const std::string v = val(); if (v != "on" && v != "off") { std::fprintf(stderr, "bad --cpu-adaptive %s (on or off)\n", v.c_str()); return 2; } cfg.p3_request_long = v == "on"; }
		else if (a == "--rx-noise") rx_noise = std::atof(val());
		else if (a == "--telnet") { telnet.port = std::atoi(val()); telnet.port_given = true; if (telnet.port < 0 || telnet.port > 65535) { std::fprintf(stderr, "bad --telnet\n"); return 2; } }
		else if (a == "--telnet-bind") telnet.bind_addr = val();
		else if (a == "--telnet-auth") telnet.password = val();
		else if (a == "--quiet") quiet = true;
		else if (a == "--version") {
			std::printf("Signum %s\nCopyright (C) 2026 Jeff Francis, N0GQ\n"
			            "License GPLv3+: GNU GPL version 3 or later <https://www.gnu.org/licenses/gpl-3.0.html>.\n"
			            "This is free software: you are free to change and redistribute it.\n"
			            "There is NO WARRANTY, to the extent permitted by law.\n", SIGNUM_VERSION);
			return 0;
		}
		else if (a == "--help" || a == "-h") { usage(stdout); return 0; }
#if defined(SIGNUM_CRYPTO) && !defined(SIGNUM_NO_CRYPTO)
		else if (signum::crypto::crypto_arg(a, val, crypto_opt)) {}
#endif
		else { std::fprintf(stderr, "unknown argument %s (--help)\n", a.c_str()); return 2; }
	}
	if (scfg.read) {
		std::string shown;
		for (size_t k = 0; k < n_file_args; k++) {
			const std::string &x = args_s[k];
			const bool secret = k > 0 && (args_s[k - 1] == "--telnet-auth" || args_s[k - 1] == "--web-auth" || args_s[k - 1] == "--ws-auth");
			shown += " " + (secret ? std::string("***") : x);
		}
		std::printf("[config] settings file %s:%s\n", scfg.path.c_str(), shown.empty() ? " (no options)" : shown.c_str());
	}
#if defined(SIGNUM_CRYPTO) && !defined(SIGNUM_NO_CRYPTO)
	if (crypto_opt.ack_none && crypto_opt.encryption == "on") crypto_opt.encryption = "off";      // --encryption-ack none wins over encryption = on from the file (verify needs none)
	if (crypto_opt.keygen || crypto_opt.show_key || (crypto_opt.encryption == "on" && signum::crypto::upper(crypto_opt.ack) != signum::crypto::kAck)) {
		signum::crypto::CryptoControl once;            // the one-shot key commands, or the refusal, before any audio
		std::string call = cfg.mycall;
		for (auto &c : call) c = char(std::toupper(uint8_t(c)));
		return std::max(0, signum::crypto::crypto_setup(crypto_opt, once, call));
	}
#endif
	const bool fifo = !ain.empty() || !aout.empty();
	if (fifo && (ain.empty() || aout.empty())) { std::fprintf(stderr, "the FIFO link needs --audio-in and --audio-out\n"); return 2; }
	if (!fifo && alsa.empty()) { std::fprintf(stderr, "need --alsa DEV or --audio-in/--audio-out (--help)\n"); return 2; }
	for (auto &c : cfg.mycall) c = char(std::toupper(uint8_t(c)));
	cfg.allow_p3 = mylevel >= 3;
	cfg.p1_only = mylevel == 1;
	std::setvbuf(stdout, nullptr, _IOLBF, 0);
	std::printf("signum %s: %s, %s\n", SIGNUM_VERSION, cfg.mycall.c_str(), fifo ? "FIFO link" : alsa.c_str());

	// ---- timing calibration (ALSA only: the FIFO link has no audio round trip) ----
	long prime = kPrime;
	bool self_timing = false, ac_hw = false, ac_ppm = false;
	fmodem::AutoCal ac;
	if (autocal_max_ms < 0) if (const char *e = std::getenv("AUTOCAL_HW_MAX_MS")) { autocal_max_ms = std::atof(e); if (!(autocal_max_ms > 0 && autocal_max_ms <= 1000)) { std::fprintf(stderr, "bad AUTOCAL_HW_MAX_MS (0 < N <= 1000)\n"); return 2; } }
	if (autocal_max_ms > 0) { ac.set_hw_max_ms(autocal_max_ms); std::printf("[cal] self-calibration: hardware round-trip readings above %.1f ms refused\n", autocal_max_ms); }
	auto ac_save = [&]() {
		if (ac_path.empty()) return;
		char buf[1024]; ac.to_text(buf, sizeof buf);
		const size_t sl = ac_path.rfind('/');
		if (sl != std::string::npos && sl > 0) ::mkdir(ac_path.substr(0, sl).c_str(), 0755);   // ~/.config/signum (its parent exists on any desktop)
		const std::string tmp = ac_path + ".tmp";
		if (FILE *f = std::fopen(tmp.c_str(), "w")) { std::fputs(buf, f); std::fclose(f); std::rename(tmp.c_str(), ac_path.c_str()); }
	};
	if (!fifo) {
		const bool in_file = load_cal(cal_path, cfg, !rtl_given);
		const char *ape = std::getenv("AUDIO_PPM");
		if (ppm_given) g_audio_ppm = audio_ppm_cli;
		else if (ape && *ape) g_audio_ppm = std::atof(ape);
		// Self-calibration for whatever the file and the options leave open (they win)
		ac_hw = !no_autocal && !rtl_given && !g_file_timing;
		ac_ppm = !no_autocal && !ppm_given && !(ape && *ape) && !g_file_ppm;
		if (ac_hw || ac_ppm) {
			if (ac_path.empty()) ac_path = default_autocal_path(alsa);
			if (FILE *f = ac_path.empty() ? nullptr : std::fopen(ac_path.c_str(), "r")) { char line[128]; while (std::fgets(line, sizeof line, f)) ac.from_line(line); std::fclose(f); }
			if (ac_ppm) g_audio_ppm = ac.ppm_applied;
			double h = 0;
			if (ac_hw) g_hw_latency = ac.hw_estimate(h) ? float(h) : kHwGuess;
			std::printf("[cal] self-calibration (state %s):%s%s\n", ac_path.empty() ? "not kept" : ac_path.c_str(),
			            ac_hw ? (ac.hw_estimate(h) ? " hardware round trip measured" : " hardware round trip not measured yet (a guess)") : "",
			            ac_ppm ? (ac.ppm_links ? " audio_ppm measured" : " audio_ppm not measured yet") : "");
		}
		g_skew_s = g_audio_ppm != 0 ? 2.0 * fmodem::ClockSkew::H / 9600.0 : 0.0;
		std::printf("[cal] %s; audio_ppm %.1f\n", in_file ? ("calibration " + cal_path).c_str() : ("no calibration file (" + cal_path + ")" + (ac_hw ? std::string(": self-calibrating") : std::string(": as callee P3 set-up may fail"))).c_str(), g_audio_ppm);
		if (prime_ms > 0) prime = std::max(48L, long(std::lround(prime_ms * 9.6)));
		self_timing = !rtl_given && g_hw_latency >= 0;
		if (self_timing) {
			const float rt = float(prime) / 9600.0f + float(fmodem::Resample5::kTaps - 1) / 48000.0f + float(g_skew_s) + g_hw_latency;
			cfg.rt_latency_s = rt - kRtFromRound; cfg.reply_adjust_s = kRaFromRound - rt;
			std::printf("[cal] self-timing: round trip %.1f ms expected: rt_latency %.4f s, reply_adjust %.4f s\n", rt * 1000.0, cfg.rt_latency_s, cfg.reply_adjust_s);
		} else if (prime != kPrime || g_skew_s > 0) {
			const float dp = float(kPrime - prime) / 9600.0f - float(g_skew_s);
			cfg.rt_latency_s -= dp; cfg.reply_adjust_s += dp;
		}
		cfg.realtime = true;
		cfg.paced = true;
		// Fix 243: with a playback path longer than the default prime, key the radio that much
		// later, so it keys as long before our audio as with the default (it used to key a whole
		// playback path early, deaf to answers still arriving: as caller our own key-up cut the
		// callee's answer once the prime passed ~60 ms). The modem's receive blanking moves with
		// it. SIGNUM_PTT_EARLY=1: the old way (key at once).
		if (prime > kPrime && !std::getenv("SIGNUM_PTT_EARLY")) {
			g_key_delay = prime - kPrime;
			cfg.rx_mute_delay = int(g_key_delay);
			std::printf("[ptt] keyed %.1f ms after our audio starts (the playback path is %.1f ms longer than the default)\n", g_key_delay / 9.6, g_key_delay / 9.6);
		}
	}

	// The radio's own tune cycle (--tune-method): only with rigctld control, which is the rig PTT's rigctld
	std::unique_ptr<radio::RigTune> rig_tune;
	if (ptt_given && ptt_spec.kind == radio::PttKind::RIG) rig_tune.reset(new radio::RigTune(ptt_spec.rig_host, ptt_spec.rig_port));
	signum::CoreImpl core(cfg, par, SIGNUM_VERSION);
#if defined(SIGNUM_CRYPTO) && !defined(SIGNUM_NO_CRYPTO)
	if (const int rc = signum::crypto::crypto_setup(crypto_opt, core.crypto(), cfg.mycall); rc >= 0) return rc;
#endif
	core.tune().secs = tune_s; core.tune().hz = tune_hz; core.tune().db = tune_db; core.tune().method = tune_method; core.tune().radio = rig_tune.get();
	core.peer().marker = peer_marker;
	// The ALC warning (--alc-warn): reads the radio's ALC meter through the PTT's rigctld while we send P2/P3 (src/radio/rig_alc.hpp)
	std::unique_ptr<radio::RigAlc> rig_alc;
	if (ptt_given && ptt_spec.kind == radio::PttKind::RIG) rig_alc.reset(new radio::RigAlc(ptt_spec.rig_host, ptt_spec.rig_port));
	core.alc().enabled = alc_warn; core.alc().reader = rig_alc.get();
	if (alc_warn && rig_alc) std::printf("[alc] ALC warning on: the radio's ALC meter is read through rigctld (%s) while we send P2/P3; it only warns (--alc-warn off to stop)\n", ptt_spec.text().c_str());
	if (!peer_marker) std::printf("[peer] peer marker off: other Signums are recognised only on encrypted links or by their own marker\n");
	if (tune_s > 0) std::printf("[tune] tuning before each new call: up to %d s, method %s (%s); the tone is %d Hz, %d dB against the data level\n", tune_s,
	                            signum::TuneTone::method_name(tune_method), rig_tune ? "rigctld control" : "no rigctld control: the tone", tune_hz, tune_db);
	core.alsa = fifo ? "fifo" : alsa;
	core.ptt = ptt_given ? ptt_spec.text() : "none";
	core.pty = pty_path;
	core.max_key_s = max_key_s;
	core.on_max_key = [](double s) { g_ptt.set_max_key(s); };
	core.on_hold_ptt = [&core](bool h) {
		g_ptt.hold(h);
		if (core.log_out) core.log_out(h ? "[ptt] held unkeyed: the radio's settings are being changed (IC-7300 set-up)\n" : "[ptt] released\n");
	};
	{
		// What this run was started with (the settings file's and the command line's options, the
		// last of each), for the Configure tab's "now" beside each next-start setting.
		std::vector<std::pair<std::string, std::string>> running;
		auto put = [&](const std::string &k, const std::string &v) {
			for (auto &kv : running) if (kv.first == k) { kv.second = v; return; }
			running.emplace_back(k, v);
		};
		for (size_t k = 0; k < args_s.size(); k++) {
			const std::string &x = args_s[k];
			if (x.size() < 3 || x[0] != '-' || x[1] != '-') continue;
			const std::string key = x.substr(2);
			if (key == "rts" && k + 1 < args_s.size()) { put("ptt", "rts:" + args_s[++k]); continue; }
			if ((key == "web-auth-file" || key == "ws-auth-file") && k + 1 < args_s.size()) { put(key.substr(0, key.size() - 5), "(from " + args_s[++k] + ")"); continue; }
			const signum::StartOpt *o = signum::StartConfig::find(key);
			if (!o) continue;
			if (o->kind == signum::StartOpt::FLAG) { put(key, "on"); continue; }
			if (k + 1 < args_s.size()) put(key, args_s[++k]);
		}
		auto mask = [](const std::string &k, const std::string &v) {
			if (k == "telnet-auth") return std::string("***");
			if ((k == "web-auth" || k == "ws-auth") && v.rfind("(from ", 0) != 0) return v.substr(0, v.find(':')) + ":***";
			return v;
		};
		auto obj = [mask](const std::vector<std::pair<std::string, std::string>> &m) {
			std::string j = "{";
			for (const auto &kv : m) j += (j.size() > 1 ? "," : "") + jsonm::quote(kv.first) + ":" + jsonm::quote(mask(kv.first, kv.second));
			return j + "}";
		};
		core.startup_json_fn = [&scfg, running, obj]() {
			std::string opts;
			for (const auto &o : signum::start_opts()) opts += (opts.empty() ? "" : ",") + jsonm::quote(o.key);
			return "{\"file\":" + jsonm::quote(scfg.path) + ",\"read\":" + (scfg.read ? "true" : "false") + ",\"values\":" + obj(scfg.values) +
			       ",\"running\":" + obj(running) + ",\"options\":[" + opts + "]}";
		};
		core.startup_set_fn = [&scfg](const std::string &k, const std::string &v) {
			return scfg.set(k, v, [](const std::string &x) { radio::PttSpec p; return p.parse(x); });
		};
#if defined(SIGNUM_CRYPTO) && !defined(SIGNUM_NO_CRYPTO)
		// The NOT-AMATEUR acknowledgement lives in the settings file, once given (docs/md/encryption.md "Acknowledgement").
		{
			auto sset = [&scfg](const std::string &k, const std::string &v) {
				return scfg.set(k, v, [](const std::string &x) { radio::PttSpec p; return p.parse(x); });
			};
			auto &cc = core.crypto();
			cc.ack_stored = [&scfg]() { return signum::crypto::upper(scfg.get("encryption-ack")) == signum::crypto::kAck; };
			cc.store_ack = [&scfg, sset](bool on) -> std::string {
				if (scfg.path.empty()) return on ? "" : "no settings file this run";
				if (on) return sset("encryption-ack", signum::crypto::kAck);
				std::string e = scfg.get("encryption") == "on" ? sset("encryption", "") : std::string();   // encryption = verify needs no acknowledgement: kept
				if (e.empty()) e = sset("encryption-ack", "");
				return e;
			};
			if (crypto_opt.ack_none) {
				if (!scfg.path.empty() && (cc.acked() || scfg.get("encryption") == "on")) {
					const std::string e = cc.store_ack(false);
					if (!e.empty()) { std::fprintf(stderr, "cannot withdraw the acknowledgement: %s\n", e.c_str()); return 2; }
				}
				std::printf("[crypto] acknowledgement withdrawn (encryption-ack and encryption removed from the settings file); encryption is off\n");
			} else if (signum::crypto::upper(crypto_opt.ack) == signum::crypto::kAck && !cc.acked() && !scfg.path.empty()) {
				const std::string e = cc.store_ack(true);
				if (e.empty()) std::printf("[crypto] acknowledgement stored in %s: encryption can now be turned on and off without it\n", scfg.path.c_str());
				else std::fprintf(stderr, "[crypto] the acknowledgement was not stored (%s)\n", e.c_str());
			}
		}
#endif
	}
	if (!fifo) { g_so = g_out.add(stdout); g_out.start(); }        // ALSA: the console from a background thread, never the audio loop
	core.log_out = [&](const std::string &s) {
		if (quiet) return;
		if (fifo) { std::fputs(s.c_str(), stdout); }
		else g_out.say_str(g_so, s);
	};
	Pty pty;
	if (pty_path != "none" && !pty.open_at(pty_path)) return 1;
	{
		std::string err;
		if (!signum::frontend_start(core, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
		if (!telnet.start(core, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
	}
	g_ppid0 = getppid();
	for (int s : {SIGINT, SIGTERM, SIGHUP}) std::signal(s, on_stop_signal);   // (before PR_SET_PDEATHSIG: never killed unlogged)
	std::signal(SIGALRM, on_alarm);
	std::signal(SIGPIPE, SIG_IGN);
	for (int s : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT}) std::signal(s, on_crash);
	const bool can_key = ptt_given && ptt_spec.kind != radio::PttKind::NONE;   // (VOX keys on our audio: it too ends with its parent)
	if (ptt_given) {
		if (!g_ptt.open(ptt_spec, max_key_s)) return 1;   // RTS/DTR held low from here on (and not raised by the open: port at B0)
		std::printf("[ptt] %s, key-down watchdog %.0f s\n", ptt_spec.text().c_str(), max_key_s);
		if (ptt_spec.kind == radio::PttKind::VOX) {
			// VOX cannot meet the turnaround: the far station answers ~0.14 s after our packet ends,
			// inside any VOX hang time, so its answers are never heard (an operator with a VOX-keyed
			// interface: the far station answered every call, we heard none)
			static const char w[] = "[ptt] WARNING: --ptt vox cannot work with P1/P2/P3: the far station answers about 0.14 s after our packet ends, "
			                        "while a VOX circuit still holds the transmitter, so no answer is ever heard. Key by rts:, dtr:, civ: or rig: instead (docs/md/radio-setup.md \"PTT\")\n";
			std::fputs(w, stdout);
			if (!isatty(1) || !isatty(2)) std::fputs(w, stderr);
		}
		std::atexit(unkey_exit);
		// a station that can key dies with its parent (no orphan holding a radio); with
		// --ptt none nothing can key, so it may outlive its launcher (signum-loopback)
		if (ptt_spec.kind != radio::PttKind::NONE) {
			prctl(PR_SET_PDEATHSIG, SIGTERM);
			if (getppid() != g_ppid0) raise(SIGTERM);      // it exited already, before the request
		}
		g_ptt.set(false, mono_s());
	}

	uint64_t n = 0;
	int16_t rxp[kPeriod], txp[kPeriod];
	// SIGNUM_REC_AUDIO=FILE.wav[:MB]: record what the modem hears and sends (src/audio_rec.hpp; off by default)
	signum::AudioRec rec;
	auto fmt_t = [](uint64_t k) { static char b[24]; std::snprintf(b, sizeof b, "%8.3f", double(k) / 9600.0); return b; };
	auto rec_say = [&](const std::string &m) { if (g_so >= 0) g_out.say_str(g_so, m); else { std::fputs(m.c_str(), stdout); std::fflush(stdout); } };   // (the FIFO link has no console thread)
	if (const char *e = std::getenv("SIGNUM_REC_AUDIO"); e && *e) {
		std::string m;
		const bool ok = rec.start(e, m);
		rec_say(std::string("[rec] ") + (ok ? "" : "not recording: ") + m + "\n");
	}
	// While recording, timing lines for the on-air pass (log only): how late the audio loop woke (the capture
	// queue it found beyond its period: 48 frames a ms), the longest such delay and the longest gap between
	// periods over the last 1-2 s, and the recorder's picture of the computer (load, iowait, busiest processes).
	double wk_prev_read = 0, wk_late = 0, wk_late_max[2] = {0, 0}, wk_gap_max[2] = {0, 0};
	uint64_t wk_bucket = 0;
	long wk_seen_late = 0;
	auto wk_note = [&](double t, long backlog) {      // the ALSA loop, each read
		if (n / 9600 != wk_bucket) { wk_bucket = n / 9600; wk_late_max[1] = wk_late_max[0]; wk_gap_max[1] = wk_gap_max[0]; wk_late_max[0] = wk_gap_max[0] = 0; }
		wk_late = backlog / 48.0;
		wk_late_max[0] = std::max(wk_late_max[0], wk_late);
		if (wk_prev_read > 0) wk_gap_max[0] = std::max(wk_gap_max[0], (t - wk_prev_read) * 1000.0);
		wk_prev_read = t;
	};
	auto wk_text = [&]() {
		char b[256];
		std::snprintf(b, sizeof b, "the latest wake-up %.1f ms late, the latest 1-2 s at most %.1f ms late, the longest gap between periods %.1f ms (%.1f expected)",
		              wk_late, std::max(wk_late_max[0], wk_late_max[1]), std::max(wk_gap_max[0], wk_gap_max[1]), kPeriod / 9.6);
		const std::string sn = rec.snapshot();
		return std::string(b) + (sn.empty() ? "" : "; " + sn);
	};
	auto period_done = [&]() {
		if (rec.on()) {
			rec.push(rxp, txp, kPeriod);
			if (const long late = core.st().decode_stats().late; late != wk_seen_late) {   // a late decode (the modem logs its own timing)
				if (late > wk_seen_late) rec_say(std::string(fmt_t(n)) + " [rt] at the late decode: " + wk_text() + "\n");
				wk_seen_late = late;
			}
			char b[160];
			if (rec.take_full_note()) { std::snprintf(b, sizeof b, "%8.3f [rec] the recording reached its size limit: stopped (the station goes on)\n", n / 9600.0); rec_say(b); }
			if (n % 9600 < kPeriod) if (const uint64_t d = rec.take_dropped()) { std::snprintf(b, sizeof b, "%8.3f [rec] %llu frames not recorded (the disk did not keep up)\n", n / 9600.0, (unsigned long long)d); rec_say(b); }
		}
		core.service(n, rxp, txp, kPeriod);
		pty.fd >= 0 ? pty.pump(core) : void();
		signum::frontend_pump(core);
		telnet.pump();
	};

	// ================= the FIFO link (lockstep) =================
	if (fifo) {
		const int fin = ::open(ain.c_str(), O_RDONLY | O_NONBLOCK);       // (the emulator's open order)
		const int fout = ::open(aout.c_str(), O_WRONLY);
		if (fin < 0 || fout < 0) { std::perror("FIFO"); return 1; }
		fcntl(fin, F_SETFL, fcntl(fin, F_GETFL) & ~O_NONBLOCK);
		int16_t rx = 0;
		bool peer_gone = false, peer_seen = false;
		int peer_wait = 0;
		uint64_t z = 0x2545F4914F6CDD1Dull;
		const double t0 = mono_s();
		const uint64_t total = duration > 0 ? uint64_t(duration * 9600.0) : UINT64_MAX;
		bool keyed = false;
		while (!g_stop && n < total) {
			for (size_t i = 0; i < kPeriod; i++, n++) {
				int16_t tx = core.tick(rx);
				rxp[i] = rx; txp[i] = tx;
				(void)!::write(fout, &tx, 2);
				if (peer_gone) { rx = 0; continue; }
				size_t got = 0;
				while (got < 2) {
					const ssize_t r = ::read(fin, reinterpret_cast<char *>(&rx) + got, 2 - got);
					if (r == 0 && !peer_seen && peer_wait++ < 10000) { ::usleep(1000); continue; }
					if (r <= 0) { peer_gone = true; rx = 0; std::printf("[fifo] the peer is gone: silence from here on\n"); break; }
					peer_seen = true;
					got += size_t(r);
				}
				if (rx_noise > 0) {
					auto u = [&]() { z ^= z << 13; z ^= z >> 7; z ^= z << 17; return (double(z >> 11) + 0.5) / 9007199254740992.0; };
					rx = int16_t(std::max(-32767.0, std::min(32767.0, rx + rx_noise * std::sqrt(-2 * std::log(u())) * std::cos(2 * M_PI * u()))));
				}
			}
			if (ptt_given) g_ptt.external_tx(core.tune().radio_tuning(), mono_s());
			if (ptt_given) { const bool want = core.key(); if (want != keyed) { keyed = want; core.alc_key(keyed); } g_ptt.set(keyed, mono_s()); }
			if (ptt_given && n % 960 == 0) { g_ptt.freq_poll(core.rig_standby()); core.rig_freq(g_ptt.freq_hz(mono_s())); }   // the panel's frequency (rig:)
			period_done();
			if (pace) {                                // real time: wait for the wall clock
				const double ahead = double(n) / 9600.0 - (mono_s() - t0);
				if (ahead > 0.0005) std::this_thread::sleep_for(std::chrono::microseconds(long(ahead * 1e6)));
			}
		}
		if (g_stop) std::fputs(stop_text(can_key).c_str(), stdout);
		std::printf("[fifo] done after %.1f s of audio\n", n / 9600.0);
		signum::frontend_stop();
		telnet.stop();
		return 0;
	}

	// ================= ALSA (real time) =================
	mlockall(MCL_CURRENT | MCL_FUTURE);
	sched_param sp{}; sp.sched_priority = 80;
	// SIGNUM_TEST_NO_RT=1 (tests only): as if the program had no real-time permission (with SIGNUM_WORKER_PRIO=0
	// for the decode workers too), so busy loops can starve it
	const bool rt_ok = !std::getenv("SIGNUM_TEST_NO_RT") && sched_setscheduler(0, SCHED_FIFO, &sp) == 0;
	if (!rt_ok) std::printf("[rt] no real-time priority (run with CAP_SYS_NICE / rtprio for the best timing)\n");
	core.health().set_no_realtime(!rt_ok);             // the CPU health indicator's advice (src/cpu_health.hpp)
	core.load_probe = [](double &l, int &cores) {      // the load average, every 5 s of audio, for the same
		double a[1];
		if (getloadavg(a, 1) != 1) return false;
		l = a[0]; cores = int(sysconf(_SC_NPROCESSORS_ONLN));
		return true;
	};
	{                                                  // the CPU clock while a link is up (src/util/cpu_clock.hpp)
		std::string msg;
		std::atexit(cpu_exit);
		g_cpu.open(cpu_mode, msg);
		if (!msg.empty()) std::printf("%s\n", msg.c_str());
		if (g_cpu.unpinnable()) core.health().set_clock_unpinned(true, "the CPU clock cannot be held up (scaling_min_freq is not writable)");
		else if (cpu_mode == signum::CpuClock::OFF) core.health().set_clock_unpinned(true, "Signum does not hold the CPU clock up (--cpu-clock off: the governor decides)");
	}
	snd_pcm_t *cap = open_pcm(alsa.c_str(), SND_PCM_STREAM_CAPTURE, 4800);
	// The playback buffer holds the prime and 50 ms more (fix 257): it was 100 ms whatever the prime, so a
	// prime above ~95 ms was cut to what fitted (--prime-ms 150 measured a 102 ms software round trip)
	// while the timing was set for the prime asked for. SIGNUM_PLAY_BUF_OLD=1: 100 ms always.
	const snd_pcm_uframes_t play_buf = std::getenv("SIGNUM_PLAY_BUF_OLD") ? 4800 : std::max<snd_pcm_uframes_t>(4800, snd_pcm_uframes_t(prime) * 5 + 2400);
	snd_pcm_t *play = open_pcm(alsa.c_str(), SND_PCM_STREAM_PLAYBACK, play_buf);
	{
		snd_pcm_uframes_t bs = 0, ps = 0;
		if (snd_pcm_get_params(play, &bs, &ps) == 0 && bs < snd_pcm_uframes_t(prime) * 5 + snd_pcm_uframes_t(kPeriod) * 5)
			std::printf("[rt] the sound card's playback buffer (%.1f ms) is shorter than the prime (%.1f ms) and a period: the audio path is shorter than the timing assumes\n", bs / 48.0, prime / 9.6);
	}
	fmodem::Resample5 rs_in, rs_out;
	fmodem::ClockSkew skew;
	skew.p = g_audio_ppm * 1e-6;
	bool use_skew = g_audio_ppm != 0, ac_up = false;
	MonoRate mono_rate;
	double sw_rt_last = -1;
	uint64_t resyncs = 0, xruns = 0, play_restarts = 0;
	double swrt_sum = 0; long swrt_n = 0;
	bool play_wait = false;
	long play_dropped = 0;
	double t_read = 0, work_ms = 0;                   // this period's work (read to write): a long one drains the playback queue
	double play_ref_sum = 0; long play_ref_n = 0;
	auto resync = [&](const char *why) {
		play_wait = false;
		snd_pcm_drop(cap); snd_pcm_drop(play);
		snd_pcm_prepare(cap); snd_pcm_prepare(play);
		std::vector<int16_t> pre(size_t(prime) * 5, 0);
		snd_pcm_writei(play, pre.data(), pre.size());
		snd_pcm_start(play); snd_pcm_start(cap);
		if (resyncs++) g_out.say(g_so, "[rt] RESYNC (%s)\n", why);
	};
	// A playback underrun restarts playback only, holding the round trip exactly
	// (fix 202): late periods are dropped,
	// then silence fills the queue to the round trip, then playback starts.
	auto play_ref = [&]() { return play_ref_n >= 400 ? play_ref_sum / double(play_ref_n) : double(prime) * 5.0; };
	auto play_resume = [&](const std::vector<int16_t> &v) {
		const snd_pcm_sframes_t backlog = std::max<snd_pcm_sframes_t>(0, snd_pcm_avail(cap));
		const long ref = std::lround(play_ref()), fill = ref - long(backlog) - long(v.size());
		if (fill < long(v.size()) && play_dropped < 48000) { play_dropped += long(v.size()); return; }
		if (fill > 0) { std::vector<int16_t> pre(size_t(fill), 0); snd_pcm_writei(play, pre.data(), pre.size()); }
		if (!v.empty()) snd_pcm_writei(play, v.data(), v.size());
		snd_pcm_start(play);
		play_wait = false;
		g_out.say(g_so, "[rt] playback underrun: restarted (%lu so far), %.1f ms late dropped (the period's work took %.1f ms)\n", (unsigned long)play_restarts, play_dropped / 48.0, work_ms);
	};
	int16_t hw_in[kPeriod * 5];
	std::vector<int16_t> hw_out(kPeriod * 5);
	bool keyed = false;
	long unkey_in = -1;
	std::vector<uint8_t> want_hist(size_t(g_key_delay) + 1, 0);   // fix 243: the modem's keying, g_key_delay samples back
	auto tick1 = [&](int16_t in16) -> int16_t {        // one modem sample (through the clock-skew corrector when on)
		if (use_skew) return int16_t(skew.step(long(n), in16, [&](int16_t h) { return double(core.tick(h)); }));
		return core.tick(in16);
	};
	double last_good_t = -1;                           // the last full capture read (monotonic clock) and what was left queued
	long last_backlog = 0;
	// SIGNUM_TEST_HW_DELAY_MS=MS (tests only): what we hear is delayed MS ms more, as a radio's codec and
	// receiver add delay that the software round trip does not see (the loopback card has none)
	std::vector<int16_t> hw_dl;
	size_t hw_dl_i = 0;
	if (const char *e = std::getenv("SIGNUM_TEST_HW_DELAY_MS")) { const long k = std::lround(std::atof(e) * 9.6); if (k > 0 && k < 96000) hw_dl.assign(size_t(k), 0); }
	long test_stall_ms = 0; uint64_t test_stall_n = 0;   // SIGNUM_TEST_STALL=S:MS: the loop sleeps MS ms once, at S s of audio
	if (const char *e = std::getenv("SIGNUM_TEST_STALL")) { double a = 0, b = 0; if (std::sscanf(e, "%lf:%lf", &a, &b) == 2) { test_stall_n = uint64_t(a * 9600); test_stall_ms = long(b); } }
	resync("start");
	while (!g_stop) {
		const snd_pcm_sframes_t got = snd_pcm_readi(cap, hw_in, kPeriod * 5);
		if (got != snd_pcm_sframes_t(kPeriod * 5)) {
			if (g_stop) break;                         // the read was interrupted by the stop signal: no resync
			static double burst_t = -1e9; static int burst_n = 0;
			const double now = mono_s();
			if (now - burst_t > 5.0) { burst_t = now; burst_n = 0; }
			if (got == -ENODEV || ++burst_n > 100) { g_out.say(g_so, "[rt] audio device lost: exiting\n"); g_out.finish(); g_ptt.force_off(); g_cpu.restore(); return 3; }
			xruns++; resync("capture overrun");
			double lost_ms = 0;
			// The capture stream stopped and what it held is gone. The modem's clock moves on by the
			// time lost (silence in, its transmit audio for that time dropped), so its packets and
			// its looks stay on the link's grid: before, the clock simply resumed, every later
			// packet went out the stall's length late, and the far end decoded none of them (a
			// 0.2 s stall ended every link). Playback is primed again as at the start, so the round
			// trip is unchanged. SIGNUM_RESYNC_NO_SKIP=1: the old way.
			static const bool no_skip = std::getenv("SIGNUM_RESYNC_NO_SKIP") != nullptr;
			const double t_s = mono_s();
			if (last_good_t > 0 && !no_skip) {
				const long skip = std::min(10L * 9600, std::lround(((t_s - last_good_t) * 48000.0 + double(last_backlog)) / 5.0));
				lost_ms = skip / 9.6;
				for (long k = 0; k < skip; k++, n++) { const int16_t z = 0, o = tick1(0); rec.push(&z, &o, 1); }   // (recorded as heard: silence)
				g_out.say(g_so, "%8.3f [rt] the audio loop stalled: %.1f ms of capture lost, the modem's clock moved on by as much (the link keeps its timing)%s\n",
				          n / 9600.0, skip / 9.6, rt_ok ? "" : "; no real-time priority, which makes stalls likelier");
			}
			last_good_t = -1;
			core.cpu_event(signum::CpuHealth::STALL, n, lost_ms);
			if (rec.on()) { char b[128]; std::snprintf(b, sizeof b, "%8.3f [rt] stall: %.1f ms of capture lost; ", n / 9600.0, lost_ms); rec_say(b + wk_text() + "\n"); }
			core.st().note_audio_slip(); ac.interrupt(); mono_rate.reset(); continue;   // the P3 receiver widens its look for a while
		}
		t_read = mono_s();
		{ const snd_pcm_sframes_t a = snd_pcm_avail(cap); last_backlog = a > 0 ? long(a) : 0; last_good_t = t_read; }
		if (rec.on()) wk_note(t_read, last_backlog);
		if (test_stall_ms > 0 && n >= test_stall_n) {   // test hook: one stall of the audio loop
			std::this_thread::sleep_for(std::chrono::milliseconds(test_stall_ms)); test_stall_ms = 0;
		}
		if (ac_ppm && ac.ppm_links == 0) mono_rate.add(n, t_read);
		for (snd_pcm_uframes_t i = 0; i < kPeriod; i++, n++) {
			const float v = rs_in.down(&hw_in[i * 5]);
			int16_t in16 = int16_t(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
			if (!hw_dl.empty()) { std::swap(in16, hw_dl[hw_dl_i]); hw_dl_i = (hw_dl_i + 1) % hw_dl.size(); }   // (test hook)
			const int16_t o = tick1(in16);
			if (ac_ppm && core.st().link_up() && !core.st().master()) ac.tick_slave(int64_t(n), core.st().ptt(), !core.st().sending(), core.st().cycle_s() * 9600.0);
			rxp[i] = in16; txp[i] = o;
			const bool want = core.key();               // (the tuning tone before a call keys too)
			bool key_now = want;
			if (g_key_delay > 0) {
				want_hist[size_t(n % want_hist.size())] = want;
				key_now = want && want_hist[size_t((n + 1) % want_hist.size())];   // (the slot written g_key_delay samples ago)
			}
			if (want) unkey_in = -1;
			if (key_now && !keyed && ptt_given) { g_ptt.set(true, mono_s()); keyed = true; core.alc_key(true); }
			if (!want && keyed && unkey_in < 0) {
				snd_pcm_sframes_t d = 0;
				if (snd_pcm_delay(play, &d) < 0) d = 0;
				unkey_in = long(d) / 5 + long(kPeriod - i) + 1 + fmodem::Resample5::kTaps / 10 + 1;
			}
			if (unkey_in > 0 && --unkey_in == 0) { g_ptt.set(false, mono_s()); keyed = false; unkey_in = -1; core.alc_key(false); }
		}
		if (ptt_given) { g_ptt.external_tx(core.tune().radio_tuning(), mono_s()); g_ptt.set(keyed, mono_s()); }     // the watchdog's service
		if (ptt_given) { g_ptt.freq_poll(core.rig_standby()); core.rig_freq(g_ptt.freq_hz(mono_s())); }   // the panel's frequency (rig:)
		if (g_cpu.want(core.st().link_up() || core.st().setting_up()) && cpu_mode == signum::CpuClock::AUTO)
			g_out.say(g_so, "%8.3f [cpu] CPU clock %s\n", n / 9600.0, g_cpu.high() ? "at its maximum (link)" : "back to the governor's minimum (standby)");
		for (size_t i = 0; i < kPeriod; i++) rs_out.up(txp[i], &hw_out[i * 5]);
		if (play_wait) play_resume(hw_out);
		else if (snd_pcm_writei(play, hw_out.data(), hw_out.size()) < 0) {
			xruns++;
			work_ms = (mono_s() - t_read) * 1000.0;
			snd_pcm_drop(play); snd_pcm_prepare(play);
			play_restarts++; play_wait = true; play_dropped = 0;
			core.cpu_event(signum::CpuHealth::UNDERRUN, n, work_ms);
			if (rec.on()) { char b[96]; std::snprintf(b, sizeof b, "%8.3f [rt] underrun: this period's work %.2f ms; ", n / 9600.0, work_ms); rec_say(b + wk_text() + "\n"); }
			play_resume(hw_out);
			core.st().note_audio_slip();               // the restart holds the round trip, but late periods were dropped
			ac.interrupt();
		}
		// No far station measured yet: the monotonic clock's figure, once, between links
		if (ac_ppm && ac.ppm_links == 0 && !use_skew && !core.st().link_up() && !core.st().setting_up() && n % 96000 < kPeriod) {
			double e = 0, h1 = 0, h2 = 0;
			if (mono_rate.estimate(e, h1, h2) && std::fabs(e) > 3.0 && std::fabs(e) < 1000.0) {
				ac.ppm_applied = e; g_audio_ppm = e;
				skew.p = e * 1e-6; skew.start_at(long(n)); use_skew = true; g_skew_s = 2.0 * fmodem::ClockSkew::H / 9600.0;
				g_out.say(g_so, "%8.3f [cal] self-calibration: sound card %.1f ppm against the system clock (halves %.1f / %.1f): audio_ppm %.1f until a far station is measured\n", n / 9600.0, e, h1, h2, e);
				ac_save();
				core.health().set_card_ppm(e);
			}
		}
		if ((ac_hw || ac_ppm) && core.st().link_up() != ac_up) {   // self-calibration: readings at a link's start (caller) and end (slave)
			ac_up = core.st().link_up();
			if (double m0 = 0, sp0 = 0; ac_up && ac_hw && core.st().master() && !core.st().long_path() && sw_rt_last > 0 && core.st().p1_answers(m0, sp0) > 0) {   // (fix 243: no clear answer, no reading)
				double m = 0, sp = 0;
				const int k = core.st().p1_answers(m, sp);
				const double v = m - fmodem::AutoCal::kPeerReply - sw_rt_last - g_skew_s - kPeerFrame;
				bool taken = false;
				if (k >= 2 && sp <= 0.003) { taken = ac.add_hw(v); ac.add_hw(v); }   // two answers within 3 ms: two readings
				else if (k == 1) taken = ac.add_hw(v);
				g_out.say(g_so, "%8.3f [cal] self-calibration: %d answer(s) to our call at +%.4f s (spread %.1f ms), software round trip %.2f ms: hardware round trip reading %.2f ms%s\n", n / 9600.0, k, m, sp * 1000.0, sw_rt_last * 1000.0, v * 1000.0,
				           k >= 1 && !taken ? (v > ac.hw_max() ? " (not taken: above the bound, a far station that answers later than the reference unit, e.g. another Signum)" : " (not taken)") : "");
				ac_save();
			} else if (!ac_up) {
				if (ac_ppm) {
					double e = 0; char why[160];
					const bool ok = ac.link_end(e, why, sizeof why);
					g_out.say(g_so, "%8.3f [cal] self-calibration: sound card %s%s\n", n / 9600.0, why, ok ? "" : " (no reading)");
					double np = 0;
					if (ok && ac.next_ppm(e, np)) {
						e = np; ac.ppm_applied = e; g_audio_ppm = e;
						if (!use_skew) { skew.p = e * 1e-6; skew.start_at(long(n)); use_skew = true; g_skew_s = 2.0 * fmodem::ClockSkew::H / 9600.0; }
						else skew.retune(e * 1e-6);
						g_out.say(g_so, "%8.3f [cal] self-calibration: audio_ppm %.1f from now on\n", n / 9600.0, e);
					}
					if (ok) {
						core.health().set_card_ppm(ac.ppm_applied);   // the total correction in place = the card's error
					}
				}
				double h = 0;
				if (ac_hw && ac.hw_estimate(h) && std::fabs(float(h) - g_hw_latency) > 0.0002f) {
					g_hw_latency = float(h);
					g_out.say(g_so, "%8.3f [cal] self-calibration: hardware round trip %.2f ms (confirmed by %d readings)\n", n / 9600.0, h * 1000.0, fmodem::AutoCal::kConfirm);
				}
				ac_save();
			}
		}
		if (!play_wait) {                              // the software round trip (self-timing)
			snd_pcm_sframes_t dpl = 0, avc = snd_pcm_avail(cap);
			if (snd_pcm_delay(play, &dpl) == 0 && avc >= 0) {
				swrt_sum += double(dpl + avc); swrt_n++;
				if (play_restarts == 0) { play_ref_sum += double(dpl + avc); play_ref_n++; }
			}
		}
		period_done();
		if (n % 48000 < kPeriod) {
			if (swrt_n) {
				const double sw_rt_s = swrt_sum / double(swrt_n) / 48000.0 + 2.0 * (fmodem::Resample5::kTaps - 1) / 2.0 / 48000.0;
				sw_rt_last = sw_rt_s;
				if (n < 48000 + kPeriod || xruns) g_out.say(g_so, "%8.3f [rt] software round trip %.2f ms, xruns %llu\n", n / 9600.0, sw_rt_s * 1000.0, (unsigned long long)xruns);
				if (self_timing) {
					const float rt = float(sw_rt_s + g_skew_s) + g_hw_latency, rtl = rt - kRtFromRound, ra = kRaFromRound - rt;
					if ((std::fabs(rtl - core.st().rt_latency()) > 0.0005f || std::fabs(ra - core.st().reply_adjust()) > 0.0005f) && core.st().set_timing(rtl, ra))
						g_out.say(g_so, "%8.3f [cal] self-timing: round trip %.2f ms: rt_latency %.4f s, reply_adjust %.4f s\n", n / 9600.0, rt * 1000.0, rtl, ra);
				}
				swrt_sum = 0; swrt_n = 0;
			}
		}
		g_out.commit();
	}
	g_ptt.close();                                  // unkeyed, RTS/DTR low, the port left at B0
	g_cpu.restore();
	if (g_stop) g_out.say_str(g_so, stop_text(can_key));
	signum::frontend_stop();
	telnet.stop();
	g_out.finish();
	return 0;
}

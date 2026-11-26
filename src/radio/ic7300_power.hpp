// ic7300_power.hpp: set an IC-7300's RF POWER for a wanted PSK peak power (--ic7300-power W, the console's
// ICPower, the GUI's Configure » Radio & audio » Set IC-7300 power; docs/md/radio-ic7300.md "Setting the power").
//
// What the number means (owner, 2026-11-20): W is the PEAK power of our P2/P3 PSK bursts: the highest peaks reach
// it, and the radio's ALC never squashes them (squashed peaks cost P3 SL4 and up). Our P1 call (FSK, constant
// envelope) is lower at the same drive; it is reported too.
//
// How the IC-7300 behaves (measured, docs/md/radio-ic7300.md "Setting the power"): RF POWER is a LIMIT, not a gain.
// Below it the drive alone sets the output (Signum's TX gain, PSKAmpl, the USB MOD Level, the codec's PCM
// volume); at it the ALC holds the output down and flattens PSK peaks. So:
//   - the peak power comes from the drive; this file predicts it (the model) or measures it (--measure);
//   - RF POWER is set to a limit that sits kHeadroom above the wanted peak, so the ALC never touches the peaks;
//   - if the drive gives peaks above W, or clearly below it, nothing is written: the answer says which TX gain to
//     use (never a silent cap: a limit at W would squash the peaks, a limit above them would let them through).
//
// The model (no transmission): P1 = G * 10^(drive_dB/10), drive_dB = tx_gain + 20 log10(PSKAmpl/140)
// + 20 log10(USB MOD/255), G from g_cool() (cool radio, the 2026-11-19 on-air points) to g_hot() (a hot radio, measured
// 2026-11-20 and 2026-11-23); both scale with the RF POWER setting that will be written (kGExp, 0.7.2); P3 peak = P1 * kP3, P2 peak = P1 * kP2 (Signum's own transmit audio). The drive is
// chosen for the HOT end (the ALC must stay at rest on a hot radio: a squashed drive costs P3 SL4 and up on the
// air), so the check against W uses the hot end times kInterSample; the peaks and P1 are reported as a range,
// cool to hot. A cool radio then gives less than W: --measure finds what this radio really gives.
// The limit curve: watts = 100 * (setting/255)^kLimitExp (the 2026-10-15 record: 20 % gave 13-14 W, 50 % gave
// 41-49 W). Every constant below says whether it is measured or inferred.
//
// The drive (owner, 2026-11-20): "ALC" is not a radio setting but the meter of the transmitter's automatic level
// control acting; what decides it is the drive (Signum's TX gain, PSKAmpl, the radio's USB MOD Level). For PSK the
// right ALC is at rest on the burst peaks. So the action sets the drive as well as RF POWER:
//   - the model (no --measure): the station default, TX gain kTxGain with USB MOD Level kUsbModLevel (both station
//     IC-7300s, measured on the air 2026-11-19), kept only when even a HOT radio (g_hot) leaves every peak under W
//     (since 0.7.2 never in 1-100 W); otherwise the TX gain the model names (0.5 dB steps, rounded down: never a
//     peak above W on a hot radio; 20 W gives -5.5 dB, 25 W -7.5 dB; --measure found -7.0 on 2026-11-23);
//   - --measure (transmits; only when the user asks): measure() below finds the TX gain on the air, the ALC at rest,
//     keying the radio over CI-V (1C 00 01) for at most MeasureLimits::key_s per step while a steady tone at exactly
//     our P1 level plays, reading Po (15 11) and ALC (15 13), unkeying (1C 00 00, read back). RF POWER is the final
//     one from the first key-down on, the drive starts low and steps up. Any error, a stop request, or the step/time limits: unkeyed, RF POWER and the USB
//     MOD Level put back, TX gain unchanged. Every key-down is also covered by a separate watchdog process
//     (key_guard.hpp) that unkeys the radio if this process dies or the key-down outlives its deadline.
// The USB MOD Level is written (backed up first) when it differs from the set-up's value; the TX gain chosen is
// returned for the caller to apply live and save to the settings file.
//
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
#pragma once

#include "ic7300.hpp"
#include "key_guard.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace radio {
namespace ic7300 {
namespace power {

// ---- the model's constants ----------------------------------------------------------------------------
// P1 (tone / FSK) watts at drive 0 dB (TX gain 0, PSKAmpl 140, USB MOD Level 255, PCM playback 105 of 128):
// MEASURED on the air 2026-11-19 (IC-7300 radio 2 on a real antenna, RF POWER 51/255, Winlink RMS runs): TX gain 0 -> 4 W on the
// tuning tone (ALC 0); TX gain +4 -> 11.5 W (ALC 0-0.1), i.e. 4.6 W at 0 dB. Two points, one radio, one band.
constexpr double kGLow = 4.0, kGHigh = 4.6;
// The "hot" G: MEASURED 2026-11-20 (IC-7300 radio 1, 80/40/20 m, the radio's own Po meter): about 15.6 W of P1 at
// 0 dB (TX gain 0), 3.4 times the cool 4.6 W (5.3 dB). One radio, three bands: which radios are hot, and why, is not
// known [measured: the figure; unknown: the cause]. The model chooses the drive from this end (kGHot), so the ALC
// stays at rest on either kind of radio; the cool end is only reported.
// 2026-11-23 (radio 1, home Pi, real antenna, 3591 kHz, RF POWER 104/255, the radio's Po meter): the first --measure with the fixed RF POWER read
// 12.6 W at TX gain -7.5 and 14.0 W at -7.0 with a steady tone at Signum's P1 level (ALC 0): G at 104/255 is about 70 W at 0 dB [measured: 71 and 70 W].
// The same evening's calls read P1 12.5 W at -6 dB (G 50 W) and 25-28 W (the ceiling, flat) at -3 and -1: the larger figure is used so the hot end
// never predicts a peak under what was read. 15.6 W (2026-11-20, 46-69/255) and 1.4-1.7 W at -3 dB at 18/255 (G about 3 W) show that G is NOT a
// constant of the radio: below its limit the output also scales with RF POWER [inferred from these points: ~3 W at 18, >= 15.6 W at 46-69, 70 W at 104;
// a power law G ~ setting^1.8 fits them]. kGHot is G at RF POWER kGRefS; g_hot()/g_cool() scale it. The lowest tone (-13.5 dB, 1.4 W) read 3.5 dB
// under the linear prediction: at very low drive the output is not linear [measured: one point, meter steps 0.35 W].
constexpr double kGHot = 70.0, kGRefS = 104.0, kGCoolS = 51.0;
constexpr double kGExp = 1.8;
inline double g_scale(int s, double ref) { return std::pow(std::max(1, std::min(255, s)) / ref, kGExp); }
// The model's hot / cool G at 0 dB for the RF POWER setting s the answer will write.
inline double g_hot(int s) { return kGHot * g_scale(s, kGRefS); }
inline double g_cool(int s) { return kGHigh * g_scale(s, kGCoolS); }
// --measure starts the drive this far under the model's HOT guess at the RF POWER it measures at (inferred: covers a radio 8 dB hotter).
constexpr double kMeasureStartDb = 8.0;
// --measure: a reading under this (about two steps of the Po meter's scale) is too coarse to take G from: the drive goes up 6 dB, never more
// in one step (a reading far off the drive's prediction is not trusted).
constexpr double kMeasureMinW = 0.7, kMeasureMaxStepDb = 6.0, kMeasureJumpDb = 1.5;
// P3 / P2 peak power over P1 power at the same drive: MEASURED on Signum's own transmit audio (bench, PSKAmpl 140):
// peaks P1 4,400, P2 ~4,950, P3 ~5,440 -> (5440/4400)^2 = 1.53, (4950/4400)^2 = 1.27. A linear chain below the ALC
// keeps these ratios (INFERRED). The 2.5x of earlier records is another modem's audio and the reference unit's.
constexpr double kP3 = 1.53, kP2 = 1.27;
// Margin for peaks between samples and for meter error, used only to stay under W: INFERRED (0.5 dB).
constexpr double kInterSample = 1.12;
// The limit above the wanted peak, so the ALC (which starts a little below the limit: 11.5 W at a ~13.5 W limit
// read 0-0.1) leaves every peak alone: 1.3 = 1.1 dB, INFERRED (the owner's example: 20 W -> 27 W).
constexpr double kHeadroom = 1.3;
// The limit curve: INFERRED from two MEASURED points (2026-10-15, radio 2, 20 m, its own Po meter): 20 % -> 13-14 W,
// 50 % -> 41-49 W; 100 * x^1.24 gives 13.6 and 42.3. A setting from it is higher than a linear one would be: the
// safe side (a limit too low squashes peaks; the peaks themselves come from the drive).
constexpr double kLimitExp = 1.24;
// Accept the drive when the predicted peak lies within this many dB under W (above W never).
constexpr double kWindowDb = 2.5;
constexpr double kMinW = 1.0, kMaxW = 100.0;

struct Drive {
	double tx_gain_db = 0;      // Signum's --tx-gain / TX gain
	int pska = 140;             // PSKAmpl
	int usb_mod = 255;          // the radio's USB MOD Level, 0-255 (read over CI-V)
};
inline double drive_db(const Drive &d)
{
	if (d.usb_mod <= 0 || d.pska <= 0) return -200;
	return d.tx_gain_db + 20.0 * std::log10(d.pska / 140.0) + 20.0 * std::log10(d.usb_mod / 255.0);
}
inline double limit_w(int s) { return s <= 0 ? 0.0 : 100.0 * std::pow(std::min(255, s) / 255.0, kLimitExp); }
// The smallest setting whose model limit is >= w (1-255; 255 when w is above 100 W).
inline int setting_for(double w)
{
	if (w <= 0) return 1;
	if (w >= 100.0) return 255;
	int s = int(std::ceil(255.0 * std::pow(w / 100.0, 1.0 / kLimitExp) - 1e-9));
	while (s > 1 && limit_w(s - 1) >= w) s--;
	while (s < 255 && limit_w(s) < w) s++;
	return std::max(1, std::min(255, s));
}
// The Po meter (15 11, 0-255 BCD) in watts: Icom's CI-V reference for the IC-7300 gives 0000 = 0 %, 0143 = 50 %,
// 0213 = 100 % (of 100 W). From Icom's manual, NOT checked against a wattmeter (INFERRED scale).
inline double po_w(int raw)
{
	if (raw <= 0) return 0;
	if (raw <= 143) return raw * 50.0 / 143.0;
	return std::min(120.0, 50.0 + (raw - 143) * 50.0 / 70.0);
}
inline double db(double r) { return 10.0 * std::log10(r); }

enum class Status { OK, BELOW_RANGE, ABOVE_100W, NO_DRIVE, DRIVE_LOW, DRIVE_HIGH, LIMIT_LOW };

struct Model {
	Status status = Status::OK;
	double target = 0, drive = 0;
	bool measured = false;      // P1 from --measure, not from the model
	double k = 1.0;             // the radio's limit / the model's limit (1: not measured)
	double p1_lo = 0, p1_hi = 0, p2_lo = 0, p2_hi = 0, p3_lo = 0, p3_hi = 0, p3_guard = 0;
	int setting = 0;            // RF POWER to write (0: none)
	double limit = 0;           // its limit in watts (corrected by k)
	bool headroom_short = false;// the limit could not be kHeadroom above W (W above 100 / kHeadroom)
	double gain_change = 0;     // DRIVE_LOW / DRIVE_HIGH: change TX gain by this (dB)
	std::string why;            // not OK: why, in a sentence
};

// The model for a wanted peak. p1_measured > 0: the P1 power from --measure; k: the measured limit correction.
Model compute(double target, const Drive &d, double k = 1.0, double p1_measured = -1, int force_setting = 0);

inline std::string pct(int s) { return std::to_string(int(std::lround(s * 100.0 / 255.0))) + " %"; }

// The answer, line by line: setting, peaks, P1, ALC, assumptions (or why not, and the TX gain to use).
std::vector<std::string> report(const Model &m, const Drive &d);

// ---- the radio --------------------------------------------------------------------------------------
// One action on the radio at a time in this process (the GUI's dialog, the console's ICPower).
inline std::atomic<bool> &busy() { static std::atomic<bool> b{false}; return b; }

// The amateur bands --measure may transmit on (kHz, Region 2, the widest; 60 m: its channels' span).
bool in_band(long long hz);
inline long long freq_hz(const Bytes &b)
{
	if (b.size() != 5) return -1;
	long long f = 0;
	for (int i = 4; i >= 0; i--) { const int hi = b[size_t(i)] >> 4, lo = b[size_t(i)] & 15; if (hi > 9 || lo > 9) return -1; f = f * 100 + hi * 10 + lo; }
	return f;
}

// RF POWER written and read back (twice at most).
inline bool write_rf(Link &l, int s, std::string &why)
{
	const Item *it = item("rf_power");
	if (s < 0 || s > 255) { why = "RF POWER out of range"; return false; }
	return put(l, *it, to_bcd2(s), why);
}

// What --measure needs besides CI-V: a steady tone at an amplitude (int16 peak at 48 kHz) through the radio's
// codec. start(): playing (false: could not); stop(): silent at once.
struct Tx {
	virtual ~Tx() = default;
	virtual bool start(double amplitude, std::string &why) = 0;
	virtual void stop() = 0;
};
struct MeasureLimits {
	double key_s = 1.5;         // the longest key-down per step (hard, in this process)
	double guard_s = 1.0;       // the watchdog process unkeys this long after key_s (radio/key_guard.hpp)
	double settle_s = 0.5;      // after the key, before the meters count
	double total_s = 12.0;      // all key-downs together
	int max_steps = 8;
	int alc_quiet = 12;         // ALC raw (0-255; the zone is 0-120) at or under this: no ALC action
	double gap_s = 0.5;         // unkeyed between steps
};

// Signum's P1 level (the tuning tone at 0 dB): 4,400 at PSKAmpl 140, times TX gain.
inline double tone_amplitude(const Drive &d) { return std::min(32000.0, 4400.0 * d.pska / 140.0 * std::pow(10.0, d.tx_gain_db / 20.0)); }
// The highest TX gain --measure tries: TX gain's +20 dB, or less where the tone would clip (0.5 dB steps).
inline double max_gain(const Drive &d) { return std::min(20.0, std::floor(2.0 * 20.0 * std::log10(31000.0 / (4400.0 * d.pska / 140.0))) / 2.0); }
inline double floor_half(double x) { return std::floor(x * 2.0 + 1e-9) / 2.0; }

struct Result {
	bool ok = false;
	Model model;
	Drive drive;                // the drive the answer is for (TX gain chosen, USB MOD Level as written)
	int before = -1;            // RF POWER before (0-255)
	int after = -1;             // RF POWER read back afterwards
	bool wrote = false;
	double gain_before = 0;     // Signum's TX gain before (as given)
	double gain_after = 0;      // the TX gain to use (the caller sets it live and in the settings file)
	bool gain_changed = false;
	int usb_before = -1;        // the radio's USB MOD Level before
	int usb_after = -1;         // and after (written when it differed from the set-up's value)
	std::string backup, error;
	std::vector<std::string> lines;   // the report
	double keyed_s = 0;         // --measure: seconds keyed in all
	int steps = 0;
	int alc_max = -1;           // --measure: the ALC on the step that counted (raw 0-255)
	int measure_rf = 0;         // --measure: the RF POWER every step ran at, and the one kept
};

namespace detail {
inline void sleep_s(double s) { if (s > 0) ::usleep(useconds_t(s * 1e6)); }
inline bool meter(Link &l, uint8_t sub, int &raw) { Bytes d; if (!l.read({0x15, sub}, d) || d.size() != 2) return false; raw = bcd(d); return raw >= 0; }
// Unkey: TX off (1C 00 00) until it reads back off (a few tries). The tone first.
inline bool unkey(Link &l, Tx &tx)
{
	tx.stop();
	for (int k = 0; k < 6; k++) {
		l.write({0x1C, 0x00}, {0x00});
		Bytes s;
		if (l.read({0x1C, 0x00}, s) && s.size() == 1 && s[0] == 0) return true;
	}
	return false;
}
}  // namespace detail

// The measurement on an examined radio (TX off, in a band, USB-D, DATA MOD USB, USB MOD Level as the set-up
// writes it). It finds the TX gain whose P1 tone is just under `aim` (the P1 power whose P3 peaks, with the
// between-samples margin, are W), with the ALC at rest, and measures that P1 power.
// RF POWER IS THE FINAL ONE FROM THE FIRST KEY-DOWN ON (2026-11-20 failure, 2026-11-23 data): below its limit the IC-7300's output also
// scales with the RF POWER setting (3 W at 0 dB at 18/255, about 70 W at 104/255), so a power measured at a low setting says nothing about
// the final one. So RF POWER is written once, at the setting the answer will keep (kHeadroom x W by the limit curve), and only the drive moves:
//   - the drive starts far under the model's hot guess at that setting (kMeasureStartDb) and steps up; at a fixed RF POWER the chain is
//     linear in the drive (1 dB more TX gain, 1 dB more power), so each reading with the ALC at rest and clearly under the limit gives the
//     drive's own power, and the next TX gain is aimed 0.3-0.8 dB under `aim` (0.5 dB steps, rounded down), at most 6 dB up per step;
//   - a reading under 0.7 W is too coarse to use: 6 dB more drive;
//   - a reading that differs from the linear prediction of the previous one by more than 1.5 dB is a sign the wrong model is at work: the
//     step is then at most 2 dB, and a third such jump ends it ("not linear in the drive");
//   - a reading near the limit (> 0.75 x the corrected limit) is the drive or the limit: the ALC meter cannot tell (2026-11-20: 0-4/255
//     at 1.3x the model's limit): the drive comes down 3 dB at the same RF POWER, again each time (step/time limits, TX gain -40 dB);
//   - Po above the corrected limit corrects k (k = at least w / the model's limit); the limit shown is then k x the curve;
//   - a Po reading more than 0.5 dB above `top` (cap x 1.26, the steady tone of P3 peaks W + 2 dB) ends it at once;
//   - the ALC acting clearly under the limit, or still at the limit with the drive at -40 dB, cannot be brought
//     down: it stops.
// Every key-down is armed with the watchdog first (guard.arm), and disarmed after TX off read back.
// true: d.tx_gain_db is the chosen TX gain, p1 the measured P1 power, k the limit correction, res.measure_rf the RF POWER measured at (to keep).
bool measure(Link &l, Tx &tx, KeyGuard &guard, double target, Drive &d, const MeasureLimits &lim, const std::atomic<bool> *stop,
                    double &p1, double &k, Result &res, const Say &say, std::string &why);

// The whole action on one radio: examine (read only), the drive, the model or the measurement, then (OK) a backup
// marked restores_rf_power, the USB MOD Level (when it differs from the set-up's) and RF POWER written and read
// back. d.tx_gain_db / d.pska: Signum's TX gain and PSKAmpl now.
//   without tx (the model): the drive is the station default (TX gain kTxGain, USB MOD Level kUsbModLevel); when
//     that cannot give W without the ALC or within kWindowDb under it, the TX gain the model names instead.
//   with tx (--measure, transmits; guard required): the TX gain found by measure().
// The chosen TX gain is res.gain_after; the CALLER applies it (live and in the settings file). Nothing is written
// when the answer is not OK; a failed measurement puts RF POWER and the USB MOD Level back.
Result set_power(util::SafePort &p, const Radio &r, double target, Drive d, Tx *tx, const Say &say,
                        const MeasureLimits &lim = MeasureLimits(), const std::atomic<bool> *stop = nullptr, KeyGuard *guard = nullptr);

}  // namespace power
}  // namespace ic7300
}  // namespace radio

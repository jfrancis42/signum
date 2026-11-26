// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// The functions of ic7300_power.hpp (out of the header, so that they build in parallel).
#include "ic7300_power.hpp"

namespace radio::ic7300::power {

Model compute(double target, const Drive &d, double k, double p1_measured, int force_setting)
{
	Model m;
	m.target = target; m.drive = drive_db(d); m.k = k > 0 ? k : 1.0; m.measured = p1_measured > 0;
	char b[320];
	if (!(target >= kMinW)) { m.status = Status::BELOW_RANGE; std::snprintf(b, sizeof b, "%.1f W is below %.0f W: Signum does not set the IC-7300 that low", target, kMinW); m.why = b; return m; }
	if (target > kMaxW) { m.status = Status::ABOVE_100W; std::snprintf(b, sizeof b, "%.1f W is above the IC-7300's 100 W", target); m.why = b; return m; }
	if (d.usb_mod <= 0) { m.status = Status::NO_DRIVE; m.why = "USB MOD Level is 0: no audio reaches the transmitter"; return m; }
	const double g = std::pow(10.0, m.drive / 10.0);
	// G depends on RF POWER (kGHot above), and the RF POWER written depends on W only: the setting the answer will write
	const int s0 = force_setting > 0 ? force_setting : setting_for(target * kHeadroom / m.k);
	m.p1_lo = m.measured ? p1_measured : g_cool(s0) * g;  // cool (4.6 W at 0 dB at 51/255)
	m.p1_hi = m.measured ? p1_measured : g_hot(s0) * g;   // hot: the drive is chosen from this end
	m.p2_lo = m.p1_lo * kP2; m.p2_hi = m.p1_hi * kP2;
	m.p3_lo = m.p1_lo * kP3; m.p3_hi = m.p1_hi * kP3;
	m.p3_guard = m.p3_hi * kInterSample;
	// the TX gain change that puts the guarded peak (hot end) 0.3-0.8 dB under W (0.5 dB steps, rounded down: err low)
	const double ch = std::floor((db(target / m.p3_guard) - 0.3) * 2.0) / 2.0;
	if (m.p3_guard > target) {
		m.status = Status::DRIVE_HIGH; m.gain_change = std::min(-0.5, ch);
		std::snprintf(b, sizeof b, "the drive is too high: P3 peaks would reach %.1f W on a cool radio and %.1f W on a hot one (up to %.1f W), above %.1f W; the ALC would squash them",
		              m.p3_lo, m.p3_hi, m.p3_guard, target);
		m.why = b;
		return m;
	}
	if (m.p3_hi < target * std::pow(10.0, -kWindowDb / 10.0)) {   // even a hot radio falls short
		m.status = Status::DRIVE_LOW; m.gain_change = std::max(0.5, ch);
		std::snprintf(b, sizeof b, "the drive is too low: P3 peaks would reach only %.1f-%.1f W (cool to hot radio), %.1f dB or more under %.1f W, whatever RF POWER is",
		              m.p3_lo, m.p3_hi, db(target / m.p3_hi), target);
		m.why = b;
		return m;
	}
	const double want = target * kHeadroom;
	m.setting = force_setting > 0 ? force_setting : setting_for(want / m.k);
	m.limit = m.k * limit_w(m.setting);
	m.headroom_short = m.limit < want;
	if (m.limit < m.p3_guard) {
		m.status = Status::LIMIT_LOW; m.setting = 0;
		std::snprintf(b, sizeof b, "the radio's limit at RF POWER 100 %% (%.1f W, measured) is below the peaks (%.1f W)", m.limit, m.p3_guard);
		m.why = b;
	}
	return m;
}

std::vector<std::string> report(const Model &m, const Drive &d)
{
	std::vector<std::string> v;
	char b[400];
	std::snprintf(b, sizeof b, "Drive: TX gain %+.1f dB, PSKAmpl %d, USB MOD Level %d/255 (%s) = %+.1f dB against the measured point", d.tx_gain_db, d.pska, d.usb_mod, pct(d.usb_mod).c_str(), m.drive);
	v.push_back(b);
	if (m.status != Status::OK) {
		v.push_back("NOT SET: " + m.why + ".");
		if (m.status == Status::DRIVE_LOW || m.status == Status::DRIVE_HIGH) {
			const double ng = d.tx_gain_db + m.gain_change;
			std::snprintf(b, sizeof b, "Change TX gain by %+.1f dB (to %+.1f dB: --tx-gain %.1f, the GUI's TX gain, or tx-gain = %.1f in the settings file), then ask again.",
			              m.gain_change, ng, ng, ng);
			v.push_back(b);
			if (ng > 20.0) v.push_back("That is above TX gain's +20 dB: raise the radio's USB MOD Level (or the codec's PCM playback volume) instead.");
			if (ng < -40.0) v.push_back("That is below TX gain's -40 dB: lower the radio's USB MOD Level (or the codec's PCM playback volume) instead.");
		}
		return v;
	}
	std::snprintf(b, sizeof b, "RF POWER: %d/255 (the radio shows %s), a limit of about %.1f W%s", m.setting, pct(m.setting).c_str(), m.limit,
	              m.k != 1.0 ? " (limit curve corrected by the measurement)" : " (by the limit curve measured on 2026-10-15)");
	v.push_back(b);
	if (m.measured) std::snprintf(b, sizeof b, "P2/P3 PSK peaks: P3 %.1f W, P2 %.1f W (from the measured P1 power)", m.p3_hi, m.p2_hi);
	else std::snprintf(b, sizeof b, "P2/P3 PSK peaks: P3 %.1f W (cool radio) to %.1f W (hot radio), P2 %.1f to %.1f W (model; never above %.1f W on either)", m.p3_lo, m.p3_hi, m.p2_lo, m.p2_hi, m.target);
	v.push_back(b);
	if (m.measured) std::snprintf(b, sizeof b, "P1 (calls, FSK, steady): %.1f W measured on the radio's Po meter", m.p1_hi);
	else std::snprintf(b, sizeof b, "P1 (calls, FSK, steady): %.1f W (cool radio) to %.1f W (hot radio) on the radio's Po meter; P3 averages about 5 dB under P1", m.p1_lo, m.p1_hi);
	v.push_back(b);
	std::snprintf(b, sizeof b, "ALC: none expected, on a hot radio either: the limit is %.1f dB above the highest P3 peaks%s", db(m.limit / m.p3_hi),
	              m.headroom_short ? " (less than the 1.1 dB wanted: the IC-7300 cannot go higher; watch the ALC on P3)" : "");
	v.push_back(b);
	v.push_back(m.measured ? "Assumes: the radio's Po meter scale (Icom's CI-V reference) and Signum's P3/P1 peak ratio 1.53 (measured on its audio)."
	                       : "Assumes: PCM playback volume 105 of 128 (the bench value; Signum cannot read it over CI-V), the 2026-11-19 on-air points "
	                         "(cool 4-4.6 W at 0 dB at 51/255; hot 70 W at 104/255, measured 2026-11-23 on radio 1; both scaled by the RF POWER setting^1.8, inferred), P3/P1 peak ratio 1.53 (Signum's audio). A model: --measure checks it on the air; "
	                         "a cool radio gives less than W (down to the cool figure), never more.");
	return v;
}

bool in_band(long long hz)
{
	static const long long b[][2] = {{1800, 2000}, {3500, 4000}, {5330, 5407}, {7000, 7300}, {10100, 10150}, {14000, 14350}, {18068, 18168},
	                                 {21000, 21450}, {24890, 24990}, {28000, 29700}, {50000, 54000}};
	const double k = hz / 1000.0;
	for (const auto &x : b) if (k >= x[0] + 1.5 && k <= x[1] - 3.0) return true;   // a USB-D signal 0-3 kHz above the dial stays inside
	return false;
}

bool measure(Link &l, Tx &tx, KeyGuard &guard, double target, Drive &d, const MeasureLimits &lim, const std::atomic<bool> *stop,
                    double &p1, double &k, Result &res, const Say &say, std::string &why)
{
	const double cap = target / kP3, aim = cap / kInterSample, top = cap * 1.26;
	const double gmax = max_gain(d);
	k = 1.0; p1 = -1;
	const int s = setting_for(target * kHeadroom);               // the final RF POWER, from the first key-down on
	res.measure_rf = s;
	Drive z = d; z.tx_gain_db = 0;
	const double base = std::pow(10.0, drive_db(z) / 10.0);      // the drive factor of PSKAmpl and USB MOD Level
	double g = floor_half(db(aim / (g_hot(s) * base)) - kMeasureStartDb);
	g = std::max(-40.0, std::min(gmax, g));
	int lowered = 0, jumps = 0, quiet_reads = 0;
	double prev_g = 0, prev_w = -1;                              // the last reading taken as the drive's own
	char b[320];
	if (!write_rf(l, s, why)) { why = "RF POWER: " + why; return false; }
	for (int step = 0; step < lim.max_steps; step++) {
		if (stop && *stop) { why = "stopped"; return false; }
		if (res.keyed_s + lim.key_s > lim.total_s + 1e-9) { why = "the time limit for key-downs is used up"; return false; }
		res.steps++;
		Drive dg = d; dg.tx_gain_db = g;
		std::snprintf(b, sizeof b, "step %d: TX gain %+.1f dB, RF POWER %d/255 (limit about %.1f W), tone at P1 level for at most %.1f s", step + 1, g, s, k * limit_w(s), lim.key_s);
		say("measure", b, -1);
		if (!guard.arm(lim.key_s + lim.guard_s, why)) return false;   // no watchdog, no key-down
		if (!tx.start(tone_amplitude(dg), why)) { why = "the tone: " + why; detail::unkey(l, tx); std::string w2; guard.disarm(w2); return false; }
		const double t0 = mono();
		const Link::Res kr = l.write({0x1C, 0x00}, {0x01});
		bool okk = kr == Link::OK;
		std::vector<double> po;
		int alc_max = -1;
		std::string err;
		if (!okk) err = kr == Link::NG ? "the radio refused TX on (NG)" : "no answer to TX on";
		while (okk) {
			const double t = mono() - t0;
			if (t >= lim.key_s) break;
			if (stop && *stop) { err = "stopped"; break; }
			int rp = -1, ra = -1;
			if (!detail::meter(l, 0x11, rp) || !detail::meter(l, 0x13, ra)) { err = "no answer from the meters"; break; }
			const double w = po_w(rp);
			if (w > top * 1.12) { std::snprintf(b, sizeof b, "Po read %.1f W, more than 0.5 dB above the highest steady power allowed (%.1f W)", w, top); err = b; break; }
			if (mono() - t0 >= lim.settle_s) { po.push_back(w); alc_max = std::max(alc_max, ra); }
			detail::sleep_s(0.05);
		}
		const bool un = detail::unkey(l, tx);
		res.keyed_s += mono() - t0;
		if (!un) { why = "COULD NOT CONFIRM TX OFF: switch the radio's transmitter off by hand (TRANSMIT key / power)"; return false; }
		std::string gw2;
		if (!guard.disarm(gw2)) { why = gw2; return false; }
		if (!err.empty()) { why = err; return false; }
		if (po.empty()) { why = "no meter readings while keyed"; return false; }
		std::sort(po.begin(), po.end());
		const double w = po[po.size() / 2];
		std::snprintf(b, sizeof b, "step %d: Po %.1f W, ALC %d/255 (max), keyed %.2f s", step + 1, w, alc_max, mono() - t0);
		say("measure", b, 1);
		const double lw = k * limit_w(s);
		if (w < kMeasureMinW) {                                  // too coarse to use (or no audio): more drive, 6 dB at most
			quiet_reads++;
			if (g >= gmax || (quiet_reads >= 3 && w < 0.1)) { why = "no power out: is the audio reaching the radio (DATA MOD USB, USB-D, the codec's PCM switch on)?"; return false; }
			g = std::min(gmax, g + kMeasureMaxStepDb);
			detail::sleep_s(lim.gap_s);
			continue;
		}
		// The ALC meter cannot be trusted to show "at the limit" (2026-11-20: 0-4/255 at 1.3x the model's limit on
		// 80/40/20 m). A reading counts as the drive's own only when it is clearly under the corrected limit (<= 0.75 x).
		if (alc_max <= lim.alc_quiet && w <= 0.75 * lw) {         // the drive's own power, ALC at rest
			double maxstep = kMeasureMaxStepDb;
			if (prev_w > 0) {                                     // the linear prediction from the last reading at this RF POWER
				const double pred = prev_w * std::pow(10.0, (g - prev_g) / 10.0);
				const double off = db(w / pred);
				if (std::fabs(off) > kMeasureJumpDb) {
					jumps++;
					std::snprintf(b, sizeof b, "Po %.1f W is %+.1f dB away from what the drive step predicted (%.1f W): the output is not following the drive", w, off, pred);
					say("measure", b, 1);
					if (jumps >= 3) { why = "the output is not linear in the drive (three readings far from the prediction): not trusted, nothing set"; return false; }
				}
			}
			if (jumps) maxstep = 2.0;
			prev_g = g; prev_w = w;
			const double room = db(aim / w);
			if (w <= aim && (room < 0.55 || g >= gmax)) { p1 = w; d.tx_gain_db = g; res.alc_max = alc_max; return true; }   // 0.55: another 0.5 dB would pass `aim`
			double dg2 = floor_half(room - 0.3);
			if (w <= aim && dg2 <= 0) dg2 = 0.5;
			dg2 = std::min(dg2, maxstep);
			const double ng = std::min(gmax, g + dg2);
			if (ng < -40.0) { why = "the drive would have to go below TX gain -40 dB: lower the codec's PCM playback volume"; return false; }
			g = ng;
		} else if (w > 0.75 * lw) {                              // near (or at) the limit: drive or limit, cannot tell
			if (alc_max > lim.alc_quiet) k = std::max(0.3, std::min(3.0, w / limit_w(s)));
			else if (w > k * limit_w(s)) k = std::max(0.3, std::min(3.0, std::max(k, w / limit_w(s))));
			prev_w = -1;                                          // the next reading is a new start
			if (g <= -40.0) {
				std::snprintf(b, sizeof b, "still at the limit (%.1f W) with the drive lowered %d times, to TX gain -40 dB: it cannot be brought down (ALC %d/255)", k * limit_w(s), lowered, alc_max);
				why = b; return false;
			}
			g = std::max(-40.0, g - 3.0); lowered++;
		} else {
			std::snprintf(b, sizeof b, "the ALC acts (%d/255) at %.1f W, well under the limit (%.1f W): it is not RF POWER; check the codec's PCM playback volume and the USB MOD Level", alc_max, w, lw);
			why = b; return false;
		}
		detail::sleep_s(lim.gap_s);
	}
	why = "no steady reading under the limit within the steps allowed";
	return false;
}

Result set_power(util::SafePort &p, const Radio &r, double target, Drive d, Tx *tx, const Say &say,
                        const MeasureLimits &lim, const std::atomic<bool> *stop, KeyGuard *guard)
{
	Result res;
	Plan plan;
	std::string err;
	res.gain_before = res.gain_after = d.tx_gain_db;
	auto fail = [&](const std::string &e) { res.error = e; res.lines.push_back("NOT SET: " + e); return res; };
	if (tx && !guard) return fail("no key-down watchdog: nothing transmitted");
	if (!examine(p, r, plan, false, err, say)) return fail(err);
	auto row = [&](const char *k) -> const Row * { const Row *x = plan.row(k); return x && x->read ? x : nullptr; };
	const Row *rf = row("rf_power"), *fq = row("freq"), *dt = row("data"), *dm = row("menu.0067"), *um = row("menu.0065");
	if (!rf || !um || !dm || !dt) return fail("could not read RF POWER, USB MOD Level, DATA MOD or the DATA mode: nothing changed");
	res.before = bcd(rf->now);
	res.usb_before = res.usb_after = bcd(um->now);
	if (dm->now != Bytes{0x03}) return fail("DATA MOD is " + dm->now_s + ", not USB: Signum's audio does not drive the transmitter (run --configure-my-ic7300-and-overwrite-its-settings first). Nothing changed");
	if (dt->now.empty() || dt->now[0] != 0x01) return fail("the radio is not in a DATA mode (USB-D): Signum's audio does not drive the transmitter. Nothing changed");
	const long long hz = fq ? freq_hz(fq->now) : -1;
	char b[320];
	std::snprintf(b, sizeof b, "RF POWER now %d/255 (%s), USB MOD Level %d/255, TX gain %+.1f dB, frequency %s", res.before, pct(res.before).c_str(), res.usb_before,
	              d.tx_gain_db, hz > 0 ? (std::to_string(hz / 1000) + " kHz").c_str() : "unread");
	say("read", b, 1);
	Drive w = d;
	w.usb_mod = kUsbModLevel;
	w.tx_gain_db = kTxGain;
	res.model = compute(target, w);
	if (!tx) {
		if (res.model.status == Status::DRIVE_LOW || res.model.status == Status::DRIVE_HIGH) {
			const double ng = w.tx_gain_db + res.model.gain_change;
			if (ng >= -40.0 && ng <= 20.0) { w.tx_gain_db = ng; res.model = compute(target, w); }
		}
		if (res.model.status != Status::OK) { res.lines = report(res.model, w); res.error = res.model.why; return res; }
	} else {
		if (res.model.status == Status::BELOW_RANGE || res.model.status == Status::ABOVE_100W || res.model.status == Status::NO_DRIVE) { res.lines = report(res.model, w); res.error = res.model.why; return res; }
		if (hz <= 0) return fail("the frequency could not be read: nothing transmitted");
		if (!in_band(hz)) return fail("the radio is on " + std::to_string(hz / 1000) + " kHz, not inside an amateur band: nothing transmitted");
	}
	const std::string dir = config_dir();
	if (dir.empty()) return fail("no HOME: nowhere to keep the backup; nothing changed");
	res.backup = backup_path(dir, r.serial);
	const std::string e = write_backup(plan, res.backup, "before --ic7300-power", true);
	if (!e.empty()) { res.backup.clear(); return fail(e + ": nothing changed"); }
	say("backup", res.backup, 1);
	Link l(p, plan.probe.addr);
	std::string why;
	if (res.usb_before != kUsbModLevel) {
		if (!put(l, *item("menu.0065"), to_bcd2(kUsbModLevel), why)) { res.error = "USB MOD Level: " + why; res.lines.push_back("NOT SET: USB MOD Level: " + why + " (restore with --restore-ic7300-settings " + res.backup + ")"); return res; }
		res.usb_after = kUsbModLevel;
		std::snprintf(b, sizeof b, "USB MOD Level: %d/255 -> %d/255 (the Signum set-up's value, read back)", res.usb_before, kUsbModLevel);
		say("set", b, 1);
	}
	if (tx) {
		double p1 = -1, k = 1.0;
		std::string gw;
		bool ok = guard->start(p.fd(), plan.probe.addr, gw);
		if (!ok) why = gw;
		else ok = measure(l, *tx, *guard, target, w, lim, stop, p1, k, res, say, why);
		guard->stop();
		if (ok) res.model = compute(target, w, k, p1, res.measure_rf);
		if (!ok || res.model.status != Status::OK) {
			std::string w2, w3;
			const bool back = write_rf(l, res.before, w2);
			res.after = back ? res.before : -1;
			bool uback = true;
			if (res.usb_after != res.usb_before) { uback = put(l, *item("menu.0065"), to_bcd2(res.usb_before), w3); if (uback) res.usb_after = res.usb_before; }
			if (ok) { res.lines = report(res.model, w); res.error = res.model.why; }
			else { res.error = why; res.lines.push_back("NOT SET: --measure stopped: " + why + "."); }
			res.lines.push_back(back ? "RF POWER is back at " + std::to_string(res.before) + "/255." : "RF POWER could NOT be put back (" + w2 + "): restore it with --restore-ic7300-settings " + res.backup);
			if (!uback) res.lines.push_back("USB MOD Level could NOT be put back (" + w3 + "): restore it with --restore-ic7300-settings " + res.backup);
			res.lines.push_back("TX gain unchanged.");
			return res;
		}
	}
	if (!write_rf(l, res.model.setting, why)) { res.error = "RF POWER: " + why; res.lines.push_back("NOT SET: RF POWER: " + why); return res; }
	res.wrote = true;
	res.after = res.model.setting;
	res.drive = w;
	res.gain_after = w.tx_gain_db;
	res.gain_changed = std::fabs(res.gain_after - res.gain_before) > 1e-9;
	res.lines = report(res.model, w);
	if (tx) {
		std::snprintf(b, sizeof b, "Measured in %d step(s), %.1f s keyed in all; ALC %d/255 at the counted step (at rest).", res.steps, res.keyed_s, res.alc_max);
		res.lines.push_back(b);
	}
	std::snprintf(b, sizeof b, "TX gain: %+.1f dB -> %+.1f dB%s", res.gain_before, res.gain_after,
	              res.gain_changed ? (tx ? " (measured)" : std::fabs(res.gain_after - kTxGain) < 1e-9 ? " (the station default)" : " (the model's, for this power)")
	                               : " (unchanged)");
	res.lines.push_back(b);
	if (res.usb_after != res.usb_before) { std::snprintf(b, sizeof b, "USB MOD Level: %d/255 -> %d/255 (the Signum set-up's value)", res.usb_before, res.usb_after); res.lines.push_back(b); }
	std::snprintf(b, sizeof b, "RF POWER: %d/255 -> %d/255 (read back)", res.before, res.after);
	say("set", b, 1);
	res.ok = true;
	return res;
}

}  // namespace radio::ic7300::power

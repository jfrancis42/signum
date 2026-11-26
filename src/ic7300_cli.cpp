// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// The functions of ic7300_cli.hpp (out of the header, so that they build in parallel).
#include "ic7300_cli.hpp"

namespace signum {

bool ic7300_args(int argc, char **argv, Ic7300Args &a, std::string &err)
{
	for (int i = 1; i < argc; i++) {
		const std::string s = argv[i];
		auto val = [&](std::string &out) { if (i + 1 >= argc) { err = s + " needs a value"; return false; } out = argv[++i]; return true; };
		if (s == "--detect-ic7300") a.detect = true;
		else if (s == "--backup-ic7300-settings") a.backup = true;
		else if (s == "--configure-my-ic7300-and-overwrite-its-settings") a.configure = true;
		else if (s == "--configure-my-ic7300-for-an-external-modem-and-overwrite-its-settings") a.external = true;
		else if (s == "--i-understand-this-overwrites-my-ic7300-settings") a.scripted = true;
		else if (s == "--restore-ic7300-settings") { if (!val(a.restore)) return false; }
		else if (s == "--ic7300-serial") { if (!val(a.serial)) return false; }
		else if (s == "--config") { if (!val(a.config)) return false; }
		else if (s == "--no-config") a.config = "none";
		else if (s == "--ic7300-power") {
			std::string v;
			if (!val(v)) return false;
			char *e = nullptr;
			a.power = std::strtod(v.c_str(), &e);
			if (e == v.c_str() || *e || !(a.power >= 0)) { err = "--ic7300-power needs the wanted P2/P3 peak power in watts (e.g. 20)"; return false; }
			if (a.power == 0) a.power = 1e-9;          // asked: refused later with the reason
		}
		else if (s == "--measure") a.measure = true;
		else if (s == "--tx-gain") { if (!val(a.tx_gain)) return false; }
	}
	if (a.measure && a.power < 0) { err = "--measure goes with --ic7300-power W"; return false; }
	if (a.power < 0) a.measure = false;
	const int n = int(a.detect) + int(a.backup) + int(a.configure) + int(a.external) + int(!a.restore.empty()) + int(a.power >= 0);
	if (n > 1) { err = "--detect-ic7300, --backup-ic7300-settings, --configure-my-ic7300-and-overwrite-its-settings, --configure-my-ic7300-for-an-external-modem-and-overwrite-its-settings, --restore-ic7300-settings and --ic7300-power: one at a time"; return false; }
	if (n == 0 && (a.scripted || !a.serial.empty())) { err = "--ic7300-serial and --i-understand-this-overwrites-my-ic7300-settings go with --configure-my-ic7300-and-overwrite-its-settings, --configure-my-ic7300-for-an-external-modem-and-overwrite-its-settings or --restore-ic7300-settings"; return false; }
	return true;
}

std::vector<std::string> ic7300_port_users(const std::string &dev)
{
	std::vector<std::string> v;
	const std::string me = std::to_string(::getpid());
	if (DIR *d = ::opendir("/proc")) {
		while (dirent *e = ::readdir(d)) {
			if (e->d_name[0] < '0' || e->d_name[0] > '9' || me == e->d_name) continue;
			const std::string fdd = std::string("/proc/") + e->d_name + "/fd";
			if (DIR *f = ::opendir(fdd.c_str())) {
				while (dirent *x = ::readdir(f)) {
					char t[PATH_MAX];
					const ssize_t k = ::readlink((fdd + "/" + x->d_name).c_str(), t, sizeof t - 1);
					if (k > 0 && std::string(t, size_t(k)) == dev) {
						std::string comm = radio::ic7300::detail::slurp(std::string("/proc/") + e->d_name + "/comm");
						v.push_back(std::string(e->d_name) + " (" + comm + ")");
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

bool ic7300_confirm(const Ic7300Args &a, const char *what)
{
	if (a.scripted) { std::printf("%s: confirmed by --i-understand-this-overwrites-my-ic7300-settings\n", what); return true; }
	if (!::isatty(0)) {
		std::printf("Not confirmed: standard input is not a terminal. Run it in a terminal and type YES, or (scripts) add\n"
		            "--i-understand-this-overwrites-my-ic7300-settings. Nothing changed.\n");
		return false;
	}
	std::printf("\nType YES (in capitals) and press Enter to go ahead; anything else stops: ");
	std::fflush(stdout);
	std::string line;
	if (!std::getline(std::cin, line)) line.clear();
	while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
	if (line != "YES") { std::printf("Not confirmed: nothing changed.\n"); return false; }
	return true;
}

int ic7300_power_main(const Ic7300Args &a)
{
	namespace ic = radio::ic7300;
	namespace pw = radio::ic7300::power;
	std::setvbuf(stdout, nullptr, _IOLBF, 0);
	const ic::Say say = [](const std::string &stage, const std::string &text, int ok) {
		std::printf("  %-4s [%s] %s\n", ok > 0 ? "ok" : ok == 0 ? "FAIL" : "", stage.c_str(), text.c_str());
	};
	pw::Drive d;
	std::string gsrc = "Signum's default";
	if (!a.tx_gain.empty()) { d.tx_gain_db = std::atof(a.tx_gain.c_str()); gsrc = "--tx-gain"; }
	else if (a.config != "none") {
		StartConfig sc;
		sc.path = a.config.empty() ? StartConfig::default_path() : a.config;
		sc.load();
		if (sc.has("tx-gain")) { d.tx_gain_db = std::atof(sc.get("tx-gain").c_str()); gsrc = "the settings file " + sc.path; }
	}
	std::printf("Wanted: P2/P3 PSK peaks at %.1f W. TX gain now %+.1f dB (%s), PSKAmpl 140 (Signum's default; a different PSKAmpl: use the console's ICPower).\n",
	            a.power, d.tx_gain_db, gsrc.c_str());
	const auto all = ic::find();
	std::vector<ic::Radio> pick;
	for (const auto &r : all) if (a.serial.empty() || r.serial == a.serial) pick.push_back(r);
	if (pick.empty()) { std::fprintf(stderr, all.empty() ? "No IC-7300 found (is it switched on and plugged in?).\n" : "No IC-7300 with serial number %s here.\n", a.serial.c_str()); return 1; }
	if (pick.size() > 1) { std::fprintf(stderr, "More than one IC-7300: choose one with --ic7300-serial N.\n"); return 1; }
	const ic::Radio &r = pick[0];
	std::printf("Radio %s on %s\n", r.serial.c_str(), r.port.c_str());
	for (const auto &u : ic7300_port_users(r.dev)) std::printf("  note: %s also has %s open (do not run this while it transmits)\n", u.c_str(), r.dev.c_str());
	std::unique_ptr<AlsaTone> tone;
	if (a.measure) {
		if (r.alsa.empty()) { std::fprintf(stderr, "--measure: the radio's sound card was not found on its USB hub: nothing transmitted.\n"); return 1; }
		const std::string bar(76, '=');
		std::printf("\n%s\n  --measure TRANSMITS\n%s\n"
		            "  Signum will key this IC-7300 on its present frequency for at most %.1f s at a time (%.0f s in all) with a\n"
		            "  steady tone at Signum's P1 level, at the RF POWER it will keep, starting with the drive (TX gain) low and stepping it up until\n"
		            "  P2/P3 peaks would reach %.1f W with the ALC at rest; the steady tone stays under %.1f W. A separate watchdog\n"
		            "  process unkeys the radio if this one dies, or %.1f s into any key-down. It then sets RF POWER just above\n"
		            "  the peaks and saves the TX gain. Use a dummy load or a clear frequency you may transmit on.\n%s\n",
		            bar.c_str(), bar.c_str(), pw::MeasureLimits().key_s, pw::MeasureLimits().total_s, a.power, a.power / pw::kP3 * 1.26 * 1.12,
		            pw::MeasureLimits().key_s + pw::MeasureLimits().guard_s, bar.c_str());
		if (!ic7300_confirm(a, "transmitting for --measure")) return 1;
		tone.reset(new AlsaTone(r.alsa));
		struct sigaction sa{};
		sa.sa_handler = [](int) { ic7300_stop_flag() = true; };
		sigemptyset(&sa.sa_mask);
		for (int sg : {SIGINT, SIGTERM, SIGHUP, SIGQUIT}) sigaction(sg, &sa, nullptr);
	}
	util::SafePort port;
	radio::ForkGuard guard;                            // --measure: unkeys the radio if this process dies while keyed
	const pw::Result res = pw::set_power(port, r, a.power, d, tone.get(), say, pw::MeasureLimits(), &ic7300_stop_flag(), a.measure ? &guard : nullptr);
	std::printf("\n");
	for (const auto &l : res.lines) std::printf("  %s\n", l.c_str());
	if (!res.backup.empty()) std::printf("  Backup (puts RF POWER and the USB MOD Level back too): signum --restore-ic7300-settings %s\n", res.backup.c_str());
	if (!res.ok) { if (res.lines.empty()) std::fprintf(stderr, "  %s\n", res.error.c_str()); return 1; }
	// the TX gain chosen: into the settings file, where --tx-gain and the GUI's TX gain keep it
	char gv[24];
	std::snprintf(gv, sizeof gv, "%g", res.gain_after);
	if (a.config == "none") {
		std::printf("  TX gain %s dB: NOT saved (--no-config): start Signum with --tx-gain %s.\n", gv, gv);
	} else {
		StartConfig sc;
		sc.path = a.config.empty() ? StartConfig::default_path() : a.config;
		sc.load();
		const std::string old = sc.has("tx-gain") ? sc.get("tx-gain") : std::string("none (0 dB)");
		if (sc.has("tx-gain") && std::fabs(std::atof(old.c_str()) - res.gain_after) < 1e-9) std::printf("  Settings file %s: tx-gain = %s already.\n", sc.path.c_str(), gv);
		else {
			const std::string e = sc.set("tx-gain", gv, [](const std::string &) { return true; });
			if (e.empty()) std::printf("  Settings file %s: tx-gain %s -> %s (applies at Signum's next start; a running Signum: its GUI's TX gain).\n", sc.path.c_str(), old.c_str(), gv);
			else { std::printf("  TX gain %s dB NOT saved: %s. Start Signum with --tx-gain %s.\n", gv, e.c_str(), gv); return 1; }
		}
		if (!a.tx_gain.empty() && std::fabs(std::atof(a.tx_gain.c_str()) - res.gain_after) > 1e-9)
			std::printf("  Note: --tx-gain %s on this command line was the drive before; the drive to use is now %s dB.\n", a.tx_gain.c_str(), gv);
	}
	return 0;
}

int ic7300_main(const Ic7300Args &a)
{
	if (a.power >= 0) return ic7300_power_main(a);
	namespace ic = radio::ic7300;
	std::setvbuf(stdout, nullptr, _IOLBF, 0);
	const ic::Say say = [](const std::string &stage, const std::string &text, int ok) {
		std::printf("  %-4s [%s] %s\n", ok > 0 ? "ok" : ok == 0 ? "FAIL" : "", stage.c_str(), text.c_str());
	};
	// the backup for a restore names its radio
	ic::Backup bk;
	std::string err;
	std::string serial = a.serial;
	const ic::Profile profile = a.external ? ic::Profile::EXTERNAL : ic::Profile::SIGNUM;
	if (!a.restore.empty()) {
		if (!ic::read_backup(a.restore, bk, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
		for (const auto &w : bk.warnings) std::fprintf(stderr, "%s\n", w.c_str());
		if (!serial.empty() && serial != bk.serial) { std::fprintf(stderr, "the backup is of radio %s, not %s\n", bk.serial.c_str(), serial.c_str()); return 1; }
		serial = bk.serial;
	}
	// ---- 1. find the radios
	const auto all = ic::find();
	if (all.empty()) {
		std::fprintf(stderr, "No IC-7300 found: no /dev/serial/by-id/*IC-7300* port of a CP210x USB serial chip.\n"
		                     "Is the radio switched on and its USB cable plugged in? (`ls /dev/serial/by-id/`, `lsusb` shows 10c4:ea60)\n");
		return 1;
	}
	std::printf("IC-7300 found: %zu\n", all.size());
	for (const auto &r : all)
		std::printf("  serial %s: CI-V/keying port %s (%s), sound card %s\n", r.serial.c_str(), r.port.c_str(), r.dev.c_str(),
		            r.alsa.empty() ? "NOT FOUND on the radio's USB hub" : (r.alsa + " (\"" + r.card + "\")").c_str());
	std::vector<ic::Radio> pick;
	for (const auto &r : all) if (serial.empty() || r.serial == serial) pick.push_back(r);
	if (pick.empty()) { std::fprintf(stderr, "No IC-7300 with serial number %s here.\n", serial.c_str()); return 1; }
	if (pick.size() > 1 && !a.detect && !a.backup) {
		std::fprintf(stderr, "More than one IC-7300: choose one with --ic7300-serial N (the serial numbers above).\n");
		return 1;
	}
	// ---- 2. ask each (read only)
	int rc = 0;
	for (const auto &r : pick) {
		std::printf("\nRadio %s:\n", r.serial.c_str());
		for (const auto &u : ic7300_port_users(r.dev))
			std::printf("  note: %s also has %s open; its CI-V traffic may take some of our answers (we ask again). Opening the\n"
			            "        port clears RTS/DTR: do not run this while that program transmits on it\n", u.c_str(), r.dev.c_str());
		util::SafePort port;
		ic::Plan plan;
		bool ok = ic::examine(port, r, plan, false, err, say, profile);
		if (!ok && plan.probe.needs_b0_exit && !plan.probe.answered) {
			std::printf("  The radio does not answer at the rate its USB serial port has now. Finding its rate means setting\n"
			            "  the port's speed, and Linux then raises the port's RTS and DTR lines for about a millisecond: a\n"
			            "  radio with USB SEND set to RTS or DTR may key for that instant (no audio is sent).\n");
			if (a.detect || a.backup || !ic7300_confirm(a, "setting the port's speed")) { std::printf("  (not asked: %s)\n", err.c_str()); rc = 1; continue; }
			ok = ic::examine(port, r, plan, true, err, say, profile);
		}
		if (!ok) { std::fprintf(stderr, "  %s\n", err.c_str()); rc = 1; continue; }
		std::printf("\n  %-38s %-26s %s\n", "Setting", "Now", !a.restore.empty() ? "Backup" : a.external ? "External modem" : "Signum sets");
		for (const auto &row : plan.rows) {
			std::string nw = row.want.empty() ? (row.it->target.empty() ? "(recorded only)" : "(left as it is)") : row.new_s;
			if (!a.restore.empty()) {
				nw = row.it->target.empty() ? "(not written)" : "(not in the backup)";
				for (const auto &kv : bk.values) if (kv.first == row.it->key) nw = ic::shown(*row.it, kv.second) + (row.read && row.now == kv.second ? " (same)" : "");
			}
			std::printf("  %-38s %-26s %s\n", row.it->name, row.now_s.c_str(), nw.c_str());
		}
		if (a.backup) {
			const std::string path = ic::backup_path(ic::config_dir(), r.serial);
			const std::string why = ic::write_backup(plan, path, "a backup on request (--backup-ic7300-settings)");
			if (!why.empty()) { std::fprintf(stderr, "  %s\n", why.c_str()); rc = 1; continue; }
			std::printf("\n  Saved: %s (nothing changed on the radio)\n  To put these settings back later: signum --restore-ic7300-settings %s\n", path.c_str(), path.c_str());
			continue;
		}
		if (a.detect) {
			std::printf("\n  %d setting(s) would change. Nothing was changed (--detect-ic7300 only reads).\n", plan.changes());
			if (!r.alsa.empty()) std::printf("  Signum would use: --alsa %s --ptt rts:%s\n", r.alsa.c_str(), r.port.c_str());
			continue;
		}
		// ---- 3. the warning
		const std::string bar(76, '=');
		std::printf("\n%s\n  WARNING: THIS RECONFIGURES YOUR RADIO\n%s\n", bar.c_str(), bar.c_str());
		if (a.restore.empty()) {
			const std::string w = ic::warning_text(plan, ic::config_dir());
			for (size_t k = 0, e; k < w.size(); k = e + 1) { e = w.find('\n', k); if (e == std::string::npos) e = w.size(); std::printf("  %s\n", w.substr(k, e - k).c_str()); }
			std::printf("  %d setting(s) change (the list above). Signum's own settings file is %s.\n", plan.changes(), a.external ? "NOT touched" : "then set for this radio");
		} else {
			std::printf("  Signum is about to write the settings in %s back to this IC-7300\n"
			            "  (serial %s). The radio's present values of those settings will be LOST from the radio.\n"
			            "  %s\n", a.restore.c_str(), r.serial.c_str(),
			            bk.rf_power ? "This backup is from --ic7300-power: it puts RF POWER back too. It does not change the frequency or transmit."
			                        : "It does NOT change the transmit power or the frequency, and it does not transmit.");
		}
		std::printf("%s\n", bar.c_str());
		if (!ic7300_confirm(a, a.restore.empty() ? "set-up" : "restore")) { rc = 1; continue; }
		// ---- 4. do it
		std::printf("\n");
		const ic::Result res = a.restore.empty() ? ic::setup(port, plan, say) : ic::restore(port, plan, bk, say);
		std::printf("\n  %d written, %d read back as wanted, %d failed.\n", res.written, res.verified, res.failed);
		if (!res.backup.empty()) std::printf("  Backup of the radio's settings before: %s\n  To put them back: signum --restore-ic7300-settings %s\n", res.backup.c_str(), res.backup.c_str());
		if (!res.ok) { std::fprintf(stderr, "  %s\n", res.error.c_str()); rc = 1; continue; }
		if (!a.restore.empty()) { std::printf("  The radio is back as the backup had it.\n"); continue; }
		// ---- 5. Signum's settings file
			if (a.external) {
				std::printf("  Signum's settings file was not changed. Signum cannot key this radio now (USB SEND and USB Keying are OFF, DATA MOD is\n"
				            "  ACC): to use Signum on it again, run signum --configure-my-ic7300-and-overwrite-its-settings.\n");
				continue;
			}
		StartConfig sc;
		sc.path = a.config == "none" ? std::string() : a.config.empty() ? StartConfig::default_path() : a.config;
		sc.load();
		if (sc.path.empty()) { std::printf("  No settings file (--no-config): start Signum with these options yourself:\n"); }
		else std::printf("  Signum's settings file %s now has:\n", sc.path.c_str());
		for (const auto &kv : ic::signum_settings(r)) {
			const std::string why = sc.path.empty() ? "" : sc.set(kv.first, kv.second, [](const std::string &x) { radio::PttSpec p; return p.parse(x); });
			std::printf("    %s = %s%s\n", kv.first.c_str(), kv.second.c_str(), why.empty() ? "" : ("   (NOT written: " + why + ")").c_str());
			if (!why.empty()) rc = 1;
		}
		if (r.alsa.empty()) std::printf("    (no sound card: the radio's USB Audio CODEC was not found on its USB hub; set alsa = ... yourself)\n");
		std::printf("  Also (defaults, nothing to write): PSK level PSKAmpl 140; self-calibration on.\n");
		if (!sc.path.empty() && sc.get("call").empty()) std::printf("  Your call sign is not in it yet: add `call = YOURCALL` (or --call).\n");
		std::printf("  Next: start Signum (`signum`), then set the transmit level with the ALC meter (docs/md/radio-ic7300.md \"Transmit level\").\n");
	}
	return rc;
}

}  // namespace signum

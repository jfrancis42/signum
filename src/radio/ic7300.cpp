// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// The functions of ic7300.hpp (out of the header, so that they build in parallel).
#include "ic7300.hpp"

namespace radio::ic7300 {

bool unhex(const std::string &s, Bytes &out)
{
	out.clear();
	int nib = -1;
	for (char c : s) {
		int v;
		if (c >= '0' && c <= '9') v = c - '0';
		else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
		else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
		else if (c == ' ' || c == '\t') { if (nib >= 0) return false; continue; }
		else return false;
		if (nib < 0) nib = v; else { out.push_back(uint8_t(nib << 4 | v)); nib = -1; }
	}
	return nib < 0 && !out.empty();
}

std::vector<Radio> find(const std::string &root)
{
	using namespace detail;
	std::vector<Radio> out;
	for (const std::string &n : list(root + "/dev/serial/by-id")) {
		if (n.find("IC-7300") == std::string::npos) continue;
		Radio r;
		r.port = root + "/dev/serial/by-id/" + n;
		r.dev = real(r.port);
		if (r.dev.empty()) continue;
		const std::string tty = base(r.dev);
		const std::string usb = usb_device(real(root + "/sys/class/tty/" + tty + "/device"));
		if (usb.empty()) continue;
		const std::string vid = slurp(usb + "/idVendor"), pid = slurp(usb + "/idProduct"), ser = slurp(usb + "/serial");
		if (vid != "10c4" || pid != "ea60" || ser.find("IC-7300") == std::string::npos) continue;   // a CP210x carrying the radio's name
		const size_t k = ser.find_last_of(' ');
		r.serial = k == std::string::npos ? ser : ser.substr(k + 1);
		r.hub = parent(usb);
		for (const std::string &c : list(root + "/sys/class/sound")) {
			if (c.compare(0, 4, "card") != 0) continue;
			const std::string cu = usb_device(real(root + "/sys/class/sound/" + c + "/device"));
			if (cu.empty() || parent(cu) != r.hub) continue;
			r.card = slurp(root + "/sys/class/sound/" + c + "/id");
			if (!r.card.empty()) { r.alsa = "plughw:CARD=" + r.card; break; }
		}
		out.push_back(r);
	}
	return out;
}

int measure_rate(Link &l, std::string &how)
{
	double t0 = 1e9, t1 = 1e9;
	int got0 = 0, got1 = 0;
	for (int k = 0; k < 8 && got0 < 3; k++) { const double t = l.timed_id(0); if (t >= 0) { t0 = std::min(t0, t); got0++; } }
	const int n = 600;
	for (int k = 0; k < 6 && got1 < 2; k++) { const double t = l.timed_id(n); if (t >= 0) { t1 = std::min(t1, t); got1++; } }
	if (t0 > 1e8 || t1 > 1e8 || t1 <= t0) { how = "not measurable (no timed answer)"; return 0; }
	const double est = n * 10.0 / (t1 - t0);
	int best = 0;
	double bd = 1e9;
	for (int r : rates()) { const double d = std::fabs(std::log(est / r)); if (d < bd) { bd = d; best = r; } }
	char b[160];
	std::snprintf(b, sizeof b, "measured by timing: %d preamble bytes took %.1f ms more, about %.0f Bd", n, (t1 - t0) * 1000.0, est);
	how = b;
	return bd < std::log(1.25) ? best : 0;
}

bool probe(util::SafePort &p, const std::string &path, Probe &pr, bool allow_b0_exit, const std::function<void(const std::string &)> &say)
{
	pr = Probe();
	if (!p.open(path, 0, true)) { pr.note = p.note(); return false; }
	pr.was_b0 = p.at_b0();
	if (!p.note().empty() && say) say(p.note());
	Link l(p, 0x94);
	l.tries = 3; l.wait_s = 0.25;
	if (!pr.was_b0) {                                     // already raised by the open: sweep from rate to rate
		for (int r : rates()) {
			p.change_rate(r);
			if (const uint8_t a = l.ask_address()) { pr.answered = true; pr.addr = a; pr.baud = r; pr.how = "the port was not at B0 (first open since plug-in): swept the rates"; break; }
		}
		p.to_b0();                                       // the chip keeps the rate found
		if (!pr.answered) pr.note = "no answer at any rate (" + std::to_string(rates().size()) + " tried)";
		return pr.answered;
	}
	if (const uint8_t a = l.ask_address()) {
		pr.answered = true; pr.addr = a;
		l.addr(a); l.tries = 1;
		pr.baud = measure_rate(l, pr.how);
		return true;
	}
	pr.needs_b0_exit = true;
	if (!allow_b0_exit) { pr.note = "no answer at the port's present rate"; return false; }
	if (say) say("no answer at the port's present rate: setting the port's speed to sweep the rates (RTS/DTR rise for about a millisecond)");
	bool first = true;
	for (int r : rates()) {
		if (first) { p.leave_b0(r); first = false; } else p.change_rate(r);
		if (const uint8_t a = l.ask_address()) { pr.answered = true; pr.addr = a; pr.baud = r; pr.how = "swept the rates (the port left B0 once)"; break; }
	}
	p.to_b0();
	if (!pr.answered) pr.note = "no answer at any rate";
	return pr.answered;
}

const std::vector<Item> & items()
{
	using namespace show;
	static const std::vector<Item> t = {
	    // menu items (MENU > SET > Connectors), CI-V 1A 05 nnnn
	    {"menu.0079", "USB Keying (CW)", menu(79), {}, {0x00}, {}, keyline},
	    {"menu.0080", "USB Keying (RTTY)", menu(80), {}, {0x00}, {}, keyline},
	    {"menu.0059", "ACC/USB Output Select", menu(59), {}, {0x00}, {}, afif},
	    {"menu.0060", "ACC/USB AF Output Level", menu(60), {}, to_bcd2(191), {}, pct},
	    {"menu.0061", "ACC/USB AF SQL", menu(61), {}, {0x00}, {}, sql},
	    {"menu.0062", "ACC/USB AF Beep/Speech... Output", menu(62), {}, {0x00}, {}, onoff},
	    {"menu.0063", "ACC/USB IF Output Level", menu(63), {}, {}, {}, pct},
	    {"menu.0064", "ACC MOD Level", menu(64), {}, {}, {}, pct},   // written by the external-modem set-up only (wanted())
	    {"menu.0065", "USB MOD Level", menu(65), {}, to_bcd2(kUsbModLevel), {}, pct},
	    {"menu.0066", "DATA OFF MOD", menu(66), {}, {0x00}, {{0x01}, {0x02}}, modsrc},   // only USB is changed (to MIC): the microphone's routing stays
	    {"menu.0067", "DATA MOD", menu(67), {}, {0x03}, {}, modsrc},
	    {"menu.0068", "External Keypad VOICE", menu(68), {}, {}, {}, onoff},
	    {"menu.0069", "External Keypad KEYER", menu(69), {}, {}, {}, onoff},
	    {"menu.0070", "External Keypad RTTY", menu(70), {}, {}, {}, onoff},
	    {"menu.0071", "CI-V Transceive", menu(71), {}, {}, {}, onoff},
	    {"menu.0072", "CI-V USB->REMOTE Transceive Address", menu(72), {}, {}, {}, addr2},
	    {"menu.0073", "CI-V Output (for ANT)", menu(73), {}, {}, {}, onoff},
	    {"menu.0075", "CI-V USB Echo Back", menu(75), {}, {0x00}, {}, onoff},
	    {"menu.0076", "USB Serial Function", menu(76), {}, {0x00}, {}, serfunc},
	    {"menu.0077", "RTTY Decode Baud Rate", menu(77), {}, {}, {}, rttybaud},
	    // operating settings
	    {"mode", "Mode, filter", {0x04}, {0x06}, {0x01, 0x01}, {}, mode},
	    {"data", "DATA mode", {0x1A, 0x06}, {}, {0x01, 0x01}, {}, data},
	    {"width", "IF filter width (FIL1 in USB-D)", {0x1A, 0x03}, {}, {0x34}, {}, width},
	    {"agc", "AGC", {0x16, 0x12}, {}, {0x02}, {}, agc},
	    {"nb", "NB", {0x16, 0x22}, {}, {0x00}, {}, onoff},
	    {"nr", "NR", {0x16, 0x40}, {}, {0x00}, {}, onoff},
	    {"anotch", "Auto notch", {0x16, 0x41}, {}, {0x00}, {}, onoff},
	    {"mnotch", "Manual notch", {0x16, 0x48}, {}, {0x00}, {}, onoff},
	    {"comp", "COMP (speech compressor)", {0x16, 0x44}, {}, {0x00}, {}, onoff},
	    {"vox", "VOX", {0x16, 0x46}, {}, {0x00}, {}, onoff},
	    {"monitor", "Monitor", {0x16, 0x45}, {}, {0x00}, {}, onoff},
	    {"split", "Split", {0x0F}, {}, {0x00}, {}, split},
	    {"rit", "RIT", {0x21, 0x01}, {}, {0x00}, {}, onoff},
	    {"xit", "XIT (delta TX)", {0x21, 0x02}, {}, {0x00}, {}, onoff},
	    // recorded only: never written
	    {"freq", "Frequency (never changed)", {0x03}, {}, {}, {}, freq},
	    {"rf_power", "RF POWER (set-ups never change it)", {0x14, 0x0A}, {}, {}, {}, level},
	    {"af_gain", "AF gain", {0x14, 0x01}, {}, {}, {}, level},
	    {"rf_gain", "RF gain", {0x14, 0x02}, {}, {}, {}, level},
	    {"preamp", "Preamp", {0x16, 0x02}, {}, {}, {}, preamp},
	    {"att", "ATT", {0x11}, {}, {}, {}, att},
	    {"tbw", "TBW (transmit bandwidth)", {0x16, 0x58}, {}, {}, {}, tbw},
	    {"menu.0074", "CI-V USB Port", menu(74), {}, {}, {}, usbport},   // read only over CI-V (Icom's manual)
	    // last: what lets RTS key the transmitter
	    {"menu.0078", "USB SEND", menu(78), {}, {0x02}, {}, keyline},
	};
	return t;
}

Bytes wanted(const Item &it, Profile pr)
{
	if (pr == Profile::SIGNUM) return it.target;
	if (!std::strcmp(it.key, "menu.0064")) return to_bcd2(128);
	if (it.target.empty()) return it.target;
	if (!std::strcmp(it.key, "menu.0067")) return {0x01};
	if (!std::strcmp(it.key, "menu.0078")) return {0x00};
	if (!std::strcmp(it.key, "menu.0065")) return {};
	return it.target;
}

void read_all(Link &l, Plan &p)
{
	p.rows.clear();
	Bytes tx;
	p.tx_read = l.read_sure({0x1C, 0x00}, tx);
	p.tx = p.tx_read && !tx.empty() && tx[0] != 0;
	for (const auto &it : items()) {
		Row r;
		r.it = &it;
		r.read = l.read_sure(it.cmd, r.now);
		r.now_s = r.read ? shown(it, r.now) : "(no answer)";
		r.want = wanted(it, p.profile);
		if (!r.want.empty() && r.read) {
			bool ok = r.now == r.want;
			for (const auto &f : it.fine) ok = ok || r.now == f;
			r.change = !ok;
			r.new_s = r.change ? shown(it, r.want) : "(as it is)";
		}
		p.rows.push_back(r);
	}
}

std::string write_backup(const Plan &p, const std::string &path, const std::string &why, bool rf_power)
{
	for (size_t k = 1; (k = path.find('/', k)) != std::string::npos; k++) ::mkdir(path.substr(0, k).c_str(), 0755);   // ~/.config/signum
	const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
	FILE *f = fd < 0 ? nullptr : ::fdopen(fd, "w");
	if (!f) { if (fd >= 0) ::close(fd); return "cannot write " + path + ": " + std::strerror(errno); }
	std::fprintf(f, "# Signum: an IC-7300's settings, read over CI-V %s (%s).\n", utc_stamp(false).c_str(), why.c_str());
	std::fprintf(f, "# Put them back with:  signum --restore-ic7300-settings %s\n", path.c_str());
	std::fprintf(f, "# Each line: key = the radio's own CI-V data bytes (hex); the comment says what they mean.\n");
	std::fprintf(f, "# Lines marked \"never written\" are recorded only: a restore does not touch them.\n");
	std::fprintf(f, "serial = %s\n", p.radio.serial.c_str());
	std::fprintf(f, "port = %s\n", p.radio.port.c_str());
	std::fprintf(f, "address = %02X\n", p.probe.addr);
	std::fprintf(f, "baud = %d\n", p.probe.baud);
	if (rf_power) std::fprintf(f, "restores_rf_power = yes    # written by --ic7300-power: a restore puts RF POWER back too\n");
	for (const auto &r : p.rows) {
		if (!r.read) { std::fprintf(f, "# %s: no answer\n", r.it->key); continue; }
		const bool rf = rf_power && !std::strcmp(r.it->key, "rf_power");
		std::fprintf(f, "%s = %s    # %s: %s%s\n", r.it->key, hex(r.now).c_str(), r.it->name, r.now_s.c_str(),
		             rf ? " (restored: this backup is from --ic7300-power)" : never_written(*r.it) ? " (never written)" : "");
	}
	const bool ok = std::fflush(f) == 0 && ::fsync(fd) == 0;
	std::fclose(f);
	return ok ? "" : "cannot write " + path;
}

bool read_backup(const std::string &path, Backup &b, std::string &err)
{
	FILE *f = std::fopen(path.c_str(), "r");
	if (!f) { err = "cannot read " + path; return false; }
	char line[512];
	int n = 0;
	Bytes rf;                                            // RF POWER: restored only from an --ic7300-power backup
	while (std::fgets(line, sizeof line, f)) {
		n++;
		std::string s = line;
		const size_t hsh = s.find('#');
		if (hsh != std::string::npos) s = s.substr(0, hsh);
		const size_t eq = s.find('=');
		if (eq == std::string::npos) continue;
		auto trim = [](std::string x) { const size_t a = x.find_first_not_of(" \t\r\n"), z = x.find_last_not_of(" \t\r\n"); return a == std::string::npos ? std::string() : x.substr(a, z - a + 1); };
		const std::string k = trim(s.substr(0, eq)), v = trim(s.substr(eq + 1));
		if (k == "serial") { b.serial = v; continue; }
		if (k == "port") { b.port = v; continue; }
		if (k == "address") { b.addr = uint8_t(std::strtoul(v.c_str(), nullptr, 16)); continue; }
		if (k == "baud") { b.baud = std::atoi(v.c_str()); continue; }
		if (k == "restores_rf_power") { b.rf_power = v == "yes"; continue; }
		const Item *it = item(k);
		Bytes d;
		if (!it) { b.warnings.push_back(path + ":" + std::to_string(n) + ": unknown key " + k + " (ignored)"); continue; }
		if (!unhex(v, d)) { b.warnings.push_back(path + ":" + std::to_string(n) + ": bad value for " + k + " (ignored)"); continue; }
		if (k == "rf_power") { if (d.size() == 2 && bcd(d) >= 0 && bcd(d) <= 255) rf = d; continue; }
		if (never_written(*it)) continue;                // recorded only (power, frequency, ...)
		b.values.emplace_back(k, d);
	}
	std::fclose(f);
	if (b.rf_power && rf.empty()) { err = path + ": marked restores_rf_power but no valid rf_power line"; return false; }
	if (b.rf_power) b.values.emplace_back("rf_power", rf);
	if (b.serial.empty()) { err = path + ": no radio serial number in it (not a Signum IC-7300 backup?)"; return false; }
	if (b.values.empty()) { err = path + ": no settings in it"; return false; }
	return true;
}

std::vector<std::string> backups()
{
	std::vector<std::string> v;
	const std::string d = config_dir();
	static const std::string pre = "ic7300-backup-";
	for (const auto &n : detail::list(d)) if (n.compare(0, pre.size(), pre) == 0 && n.size() > pre.size() + 5 && n.substr(n.size() - 5) == ".conf") v.push_back(d + "/" + n);
	std::sort(v.rbegin(), v.rend());                    // newest first (the names carry the time)
	return v;
}

bool put(Link &l, const Item &it, const Bytes &v, std::string &why)
{
	for (int t = 0; t < 2; t++) {
		const Link::Res r = l.write(it.wcmd.empty() ? it.cmd : it.wcmd, v);
		if (r == Link::NG) { why = "the radio refused it (NG)"; continue; }
		if (r == Link::NONE) { why = "no answer to the write"; continue; }
		Bytes back;
		if (!l.read_sure(it.cmd, back)) { why = "written, but no answer to the read-back"; continue; }
		if (back == v) return true;
		why = "read back " + shown(it, back) + " (" + hex(back) + "), not " + shown(it, v) + " (" + hex(v) + ")";
	}
	return false;
}

void write_list(Link &l, Plan &plan, const std::vector<std::pair<const Item *, Bytes>> &w, Result &res, const Say &say, const char *stage)
{
	auto line_item = [](const Item *it) { return !std::strcmp(it->key, "menu.0078") || !std::strcmp(it->key, "menu.0079") || !std::strcmp(it->key, "menu.0080"); };
	auto one = [&](const Item &it, const Bytes &v, const char *note) {
		std::string why;
		const Row *r = plan.row(it.key);
		const std::string from = r && r->read ? r->now_s : "?";
		res.written++;
		if (put(l, it, v, why)) { res.verified++; say(stage, std::string(it.name) + ": " + from + " -> " + shown(it, v) + " (read back)" + note, 1); return true; }
		res.failed++;
		say(stage, std::string(it.name) + ": " + why, 0);
		return false;
	};
	for (const auto &x : w)
		if (line_item(x.first) && x.second != Bytes{0x00}) {
			const Row *r = plan.row(x.first->key);
			if (r && r->read && r->now != Bytes{0x00}) one(*x.first, {0x00}, " (first, so no two items share a line)");
		}
	for (const auto &x : w) if (!line_item(x.first)) one(*x.first, x.second, "");
	for (const char *k : {"menu.0079", "menu.0080", "menu.0078"})
		for (const auto &x : w) if (!std::strcmp(x.first->key, k)) one(*x.first, x.second, "");
}

bool examine(util::SafePort &p, const Radio &r, Plan &plan, bool allow_b0_exit, std::string &err, const Say &say, Profile profile)
{
	plan = Plan();
	plan.radio = r;
	plan.profile = profile;
	if (!probe(p, r.port, plan.probe, allow_b0_exit, [&](const std::string &t) { say("port", t, -1); })) {
		err = plan.probe.needs_b0_exit && !allow_b0_exit
		    ? "the radio did not answer at the port's present rate (" + plan.probe.note + ")"
		    : "no CI-V answer from the radio on " + r.port + (plan.probe.note.empty() ? "" : ": " + plan.probe.note) +
		      ". Is it switched on, and MENU > SET > Connectors > USB Serial Function set to CI-V?";
		return false;                                    // needs_b0_exit: the caller may ask the user, then call again
	}
	char ab[8];
	say("found", "CI-V address " + std::string(show::addr({plan.probe.addr}, ab, sizeof ab)) + ", " +
	    (plan.probe.baud ? std::to_string(plan.probe.baud) + " Bd" : std::string("rate not measured")) + (plan.probe.how.empty() ? "" : " (" + plan.probe.how + ")"), 1);
	Link l(p, plan.probe.addr);
	read_all(l, plan);
	int nr = 0;
	for (const auto &x : plan.rows) nr += x.read;
	say("read", std::to_string(nr) + " of " + std::to_string(plan.rows.size()) + " settings read", nr == int(plan.rows.size()) ? 1 : 0);
	if (!plan.tx_read) { err = "the radio did not answer the TX state question (1C 00)"; return false; }
	if (plan.tx) { err = "the radio is transmitting: nothing changed (try again when it is not)"; return false; }
	return true;
}

Result setup(util::SafePort &p, Plan &plan, const Say &say)
{
	Result res;
	const std::string dir = config_dir();
	if (dir.empty()) { res.error = "no HOME: nowhere to keep the backup"; return res; }
	res.backup = backup_path(dir, plan.radio.serial);
	for (const auto &r : plan.rows) if (r.want.size() && !r.read) { res.error = std::string("could not read ") + r.it->name + ": nothing changed (no complete backup)"; return res; }
	const std::string e = write_backup(plan, res.backup, plan.profile == Profile::EXTERNAL ? "before the IC-7300 set-up for an external modem" : "before Signum's IC-7300 set-up");
	if (!e.empty()) { res.error = e + ": nothing changed"; return res; }
	say("backup", res.backup, 1);
	Link l(p, plan.probe.addr);
	std::vector<std::pair<const Item *, Bytes>> w;
	for (const auto &r : plan.rows) if (r.change) w.emplace_back(r.it, r.want);
	write_list(l, plan, w, res, say, "set");
	// everything read again: the record of what the radio holds now
	Plan after = plan;
	read_all(l, after);
	int bad = 0;
	for (const auto &r : after.rows) if (r.change) { bad++; say("check", std::string(r.it->name) + " reads " + r.now_s + " afterwards", 0); }
	res.ok = res.failed == 0 && bad == 0;
	if (!res.ok && res.error.empty()) res.error = std::to_string(res.failed + bad) + " setting(s) not as wanted; the backup puts the radio back as it was";
	return res;
}

Result restore(util::SafePort &p, Plan &plan, const Backup &b, const Say &say)
{
	Result res;
	if (b.serial != plan.radio.serial) { res.error = "the backup is of radio " + b.serial + ", this is radio " + plan.radio.serial; return res; }
	Link l(p, plan.probe.addr);
	std::vector<std::pair<const Item *, Bytes>> w;
	for (const auto &it : items())
		for (const auto &kv : b.values)
			if (kv.first == it.key) {
				const Row *r = plan.row(it.key);
				if (r && r->read && r->now == kv.second) { say("restore", std::string(it.name) + ": " + shown(it, kv.second) + " already", 1); continue; }
				w.emplace_back(&it, kv.second);
			}
	write_list(l, plan, w, res, say, "restore");
	res.ok = res.failed == 0;
	if (!res.ok) res.error = std::to_string(res.failed) + " setting(s) not restored";
	return res;
}

std::vector<std::pair<std::string, std::string>> signum_settings(const Radio &r)
{
	std::vector<std::pair<std::string, std::string>> v;
	if (!r.alsa.empty()) v.emplace_back("alsa", r.alsa);
	v.emplace_back("ptt", "rts:" + r.port);
	// +4 dB: measured on the air 2026-11-19 (IC-7300, USB MOD Level 100 %, PCM 105/128, RF POWER 51/255): TX gain 0
	// gave 4 W on the tuning tone against a 20 W setting with no ALC; +4 gave 11.5 W with the ALC at 0 (+7 reaches
	// the setting; +4 keeps P3 peaks out of the ALC). docs/md/radio-ic7300.md "Transmit level".
	char g[16];
	std::snprintf(g, sizeof g, "%g", kTxGain);
	v.emplace_back("tx-gain", g);
	v.emplace_back("prime-ms", "40");
	v.emplace_back("no-autocal", "off");
	return v;
}

std::string warning_text(const Plan &p, const std::string &backup_dir)
{
	std::string s;
	s += "Signum is about to RECONFIGURE this IC-7300 (serial " + p.radio.serial + ")" +
	     (p.profile == Profile::EXTERNAL ? " for an external modem on its ACC socket: Signum will not be able to key it until the Signum set-up is run again" : "") + ".\n";
	s += "The radio's current values of the settings below will be LOST from the radio.\n";
	s += "Before changing anything, Signum saves them to a backup file in " + backup_dir + "/\n";
	s += "(ic7300-backup-" + p.radio.serial + "-<time>.conf); `signum --restore-ic7300-settings FILE` or the\n";
	s += "GUI's Restore button puts them back.\n";
	s += "It does NOT change the transmit power or the frequency, and it does not transmit.\n";
	return s;
}

}  // namespace radio::ic7300

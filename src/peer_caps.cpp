// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// The functions of peer_caps.hpp (out of the header, so that they build in parallel).
#include "peer_caps.hpp"

namespace signum {

std::string caps_body(const Caps &c)
{
	std::string b = "FE1";
	b += ";p" + c.product + ";v" + c.version;
	if (!c.enc.empty()) b += ";e" + caps_list(c.enc);
	if (!c.ver.empty()) b += ";a" + caps_list(c.ver);
	if (c.sign) b += ";s1";
	if (c.has_static) b += ";ks";
	if (!c.psk_id.empty()) b += ";q" + c.psk_id;
	if (!c.comp.empty()) {
		b += ";c";
		for (size_t i = 0; i < c.comp.size(); i++) b += (i ? "," : "") + c.comp[i];
	}
	return b.substr(0, kCapsMaxBody);
}

Caps parse_caps(const std::string &b)
{
	Caps c;
	c.valid = false;
	if (b.size() < 3 || b.size() > kCapsMaxBody || b.compare(0, 2, "FE") != 0 || !std::isdigit(uint8_t(b[2])) || b[2] == '0') return c;
	for (char x : b) if (uint8_t(x) < 0x21 || uint8_t(x) > 0x7E) return c;
	c.format = b[2] - '0';
	size_t a = 3;
	if (a < b.size() && b[a] != ';') return c;
	auto nums = [](const std::string &v, std::vector<int> &out) {
		for (auto &s : split_list(v)) {
			if (s.empty() || s.size() > 4) return false;
			for (char d : s) if (!std::isdigit(uint8_t(d))) return false;
			out.push_back(std::stoi(s));
		}
		return true;
	};
	while (a < b.size()) {
		size_t e = b.find(';', a + 1);
		if (e == std::string::npos) e = b.size();
		const std::string f = b.substr(a + 1, e - a - 1);
		a = e;
		if (f.empty()) continue;
		const char t = f[0];
		const std::string v = f.substr(1);
		if (t == 'p') c.product = v;
		else if (t == 'v') c.version = v;
		else if (t == 'e') { if (!nums(v, c.enc)) return c; }
		else if (t == 'a') { if (!nums(v, c.ver)) return c; }
		else if (t == 's') c.sign = v == "1";
		else if (t == 'k') c.has_static = v.find('s') != std::string::npos;
		else if (t == 'q') c.psk_id = v;
		else if (t == 'c') c.comp = split_list(v);
		// other tags: a newer station's; skipped
	}
	c.valid = !c.product.empty();
	return c;
}

}  // namespace signum

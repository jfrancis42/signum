// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// The functions of p3_unproto_rx.hpp (out of the header, so that they build in parallel).
#include "p3_unproto_rx.hpp"

namespace fmodem::p3u {

Packet sl1_short(const std::vector<float> &x, double fs, double s, double search_ms)
{
	Packet out;
	const double T = kT * fs, st = p3::kCsStagger * T;
	const auto &tab = p3::cs_pulse_table(float(fs));
	// timing and carrier order by the header fit on the first 10 pulses
	double best = 99, bdt = 0; int bsw = 0;
	const double span = search_ms * 1e-3 * fs, step = std::max(1.0, 0.0005 * fs);
	for (int sw = 0; sw < 2; sw++)
		for (double dt = -span; dt <= span + 1e-9; dt += step) {
			const double fA = sw ? p3::kCsHi : p3::kCsLo, fB = sw ? p3::kCsLo : p3::kCsHi;
			const auto z = p2::ls_pulses(x, fs, s + dt, s + dt + st, fA, fB, 10, &tab);
			if (z.size() < 20) continue;
			double hs[8][2];
			for (int i = 0; i < 8; i++) { hs[i][0] = nib(z[size_t(i)], z[size_t(i + 1)]); hs[i][1] = nib(z[size_t(10 + i)], z[size_t(10 + i + 1)]); }
			const auto hf = p2::fit_header(hs, 1);
			if (hf.rms < best) { best = hf.rms; bdt = dt; bsw = sw; }
		}
	if (best > 3.0) return out;
	for (int sw0 = 0; sw0 < 2; sw0++) {
		const int sw = sw0 ? !bsw : bsw;
		for (double dt : {bdt, bdt - step / 2, bdt + step / 2}) {
			const double fA = sw ? p3::kCsHi : p3::kCsLo, fB = sw ? p3::kCsLo : p3::kCsHi;
			const auto z = p2::ls_pulses(x, fs, s + dt, s + dt + st, fA, fB, 82, &tab);
			if (z.size() < 164) continue;
			double hs[8][2], steps[72][2];
			for (int i = 0; i < 8; i++) { hs[i][0] = nib(z[size_t(i)], z[size_t(i + 1)]); hs[i][1] = nib(z[size_t(82 + i)], z[size_t(82 + i + 1)]); }
			const auto hf = p2::fit_header(hs, 1);
			for (int k = 0; k < 72; k++) for (int q = 0; q < 2; q++) steps[k][q] = std::fmod(nib(z[size_t(82 * q + 8 + k)], z[size_t(82 * q + 9 + k)]) - hf.bias + 32.0, 16.0);
			const auto r = p2::decode_steps(1, steps);
			uint8_t d[p3::kSl1Data], status;
			if (r.info.size() < 8 || !p3::sl1_check(r.info.data(), d, status)) continue;
			out.ok = true; out.level = 1; out.status = status; out.data.assign(d, d + p3::kSl1Data); out.slot = s + dt; out.odd = sw;
			return out;
		}
	}
	return out;
}

Packet sl1_long(const std::vector<float> &x, double fs, double s, double search_ms)
{
	Packet out;
	const double T = kT * fs, st = p3::kCsStagger * T;
	const auto &tab = p3::cs_pulse_table(float(fs));
	const int nw = p3::lng::level(1).nwords;
	const double span = search_ms * 1e-3 * fs, step = std::max(1.0, 0.0005 * fs);
	// timing and carrier order by the fit of words 1-8, the long packet's header (the template's, by the
	// counter's parity), on the first 10 pulses
	const auto &LL = p3::lng::level(1);
	double best = 99, bdt = 0; int bsw = 0;
	for (int sw = 0; sw < 2; sw++)
		for (double dt = -span; dt <= span + 1e-9; dt += step) {
			const double fA = sw ? p3::kCsHi : p3::kCsLo, fB = sw ? p3::kCsLo : p3::kCsHi;
			const auto z = p2::ls_pulses(x, fs, s + dt, s + dt + st, fA, fB, 10, &tab);
			if (z.size() < 20) continue;
			for (int par = 0; par < 2; par++) {
				std::complex<double> acc = 0; double d[16];
				for (int i = 0; i < 8; i++)
					for (int c = 0; c < 2; c++) {
						const int want = c ? LL.tmpl[par][i + 1] & 15 : LL.tmpl[par][i + 1] >> 4;
						d[2 * i + c] = nib(z[size_t(10 * c + i)], z[size_t(10 * c + i + 1)]) - want;
						acc += std::polar(1.0, 2 * M_PI * d[2 * i + c] / 16.0);
					}
				const double bias = 16.0 * std::arg(acc) / (2 * M_PI);
				double e2 = 0; for (double v : d) { const double e = std::fmod(v - bias + 1608.0, 16.0) - 8.0; e2 += e * e; }
				const double rms = std::sqrt(e2 / 16);
				if (rms < best) { best = rms; bdt = dt; bsw = sw; }
			}
		}
	if (best > 3.0) return out;
	// the whole packet (a fit of 334 pulses: the costly part on a small computer) at the header's timing
	// and carrier order first, then half a step either side, then the other order
	std::vector<double> nb(size_t(2 * nw));
	const int N = nw + 4;
	for (int sw0 = 0; sw0 < 2; sw0++) {
		const int sw = sw0 ? !bsw : bsw;
		for (double dt : {bdt, bdt - step / 2, bdt + step / 2}) {
			const double fA = sw ? p3::kCsHi : p3::kCsLo, fB = sw ? p3::kCsLo : p3::kCsHi;
			const auto z = p2::ls_pulses(x, fs, s + dt - T, s + dt - T + st, fA, fB, N, &tab);
			if (int(z.size()) < 2 * N) continue;
			for (int o = 0; o >= -2 && o <= 2; o = o >= 0 ? -o - 1 : -o) {
				bool fits = true;
				for (int w = 0; w < nw && fits; w++) {
					const int i = w + 1 + o;
					if (i < 1 || i >= N) { fits = false; break; }
					nb[size_t(2 * w)] = nib(z[size_t(i - 1)], z[size_t(i)]);
					nb[size_t(2 * w + 1)] = nib(z[size_t(N + i - 1)], z[size_t(N + i)]);
				}
				if (!fits) continue;
				const p3::LongRx r = p3::long1_decode(nb.data());
				if (r.ok) { out.ok = true; out.level = 1; out.lng = true; out.status = r.status; out.data = r.data; out.slot = s + dt + o * T; out.odd = sw; return out; }
			}
		}
	}
	return out;
}

Packet lv_long(int lv, const std::vector<float> &x, double fs, double s, double search_ms)
{
	Packet out;
	const double base = s + 0.8 * kT * fs + pulse0_off_s(lv, true) * fs;
	const p3::LvSync sy = p3::sync_level(lv, x.data(), long(x.size()), fs, base, search_ms);
	const p3::Layout &L = p3::layout_long(lv);
	auto decode_d = [&](std::vector<std::complex<double>> &d) {
		static const int kHdr[7] = {0, 0, 6, 14, 16, 18, 18};
		const int P = lv <= 3 ? 2 : 4;
		std::complex<double> acc(0, 0);
		for (size_t w = size_t(9 * kHdr[lv]); w < d.size(); w++) { const double a = std::abs(d[w]); if (a > 0) acc += std::pow(d[w] / a, P) * a; }
		if (std::abs(acc) > 0) {
			const double m = 16.0 / P;
			double b = -(std::arg(acc) + M_PI * P / 4.0) * 16.0 / (2 * M_PI * P);
			b = std::fmod(b + 64.0 * m + m / 2, m) - m / 2;
			const std::complex<double> rot = std::polar(1.0, 2 * M_PI * b / 16.0);
			for (auto &v : d) v *= rot;
		}
		std::vector<double> stp(d.size(), 0.0), wt(d.size(), 1.0);
		double mm = 0; int n = 0;
		for (size_t w = 0; w < d.size(); w++) { const double v = -std::arg(d[w]) / (2 * M_PI) * 16; stp[w] = v < 0 ? v + 16 : v; const double a = std::abs(d[w]); if (a > 0) { mm += a; n++; } }
		if (n) for (size_t w = 0; w < d.size(); w++) wt[w] = std::abs(d[w]) / (mm / n);
		return p3::long_decode(lv, stp.data(), n ? wt.data() : nullptr);
	};
	for (int oi = 0; oi < 2; oi++) {
		const bool odd = oi ? !sy.odd : sy.odd;
		std::vector<std::complex<double>> d(size_t(L.nwords()), std::complex<double>(0, 0));
		if (!L.narrow) {
			const auto zf = p3::demod_flat(lv, x.data(), long(x.size()), fs, sy.t, 0, nullptr, -1, true);
			for (int pp = 1; pp < L.npulses(); pp++)
				for (int j = 0; j < L.carriers(pp); j++) { const int k = L.carrier(pp, j, odd); d[size_t(L.word_index(pp, j))] = zf[size_t(pp)][size_t(k)] * std::conj(zf[size_t(pp - 1)][size_t(k)]); }
		} else {
			const auto z = p3::demod_lv(lv, x.data(), long(x.size()), fs, sy.t, odd, 0, nullptr, -1, true);
			for (int pp = 1; pp < L.npulses(); pp++)
				for (int j = 0; j < L.carriers(pp); j++) {
					const int k = L.carrier(pp, j, odd);
					int jp = -1;
					for (int q = 0; q < L.carriers(pp - 1); q++) if (L.carrier(pp - 1, q, odd) == k) jp = q;
					if (jp >= 0) d[size_t(L.word_index(pp, j))] = z[size_t(pp)][size_t(j)] * std::conj(z[size_t(pp - 1)][size_t(jp)]);
				}
		}
		const p3::LongRx r = decode_d(d);
		if (r.ok) { out.ok = true; out.level = lv; out.lng = true; out.status = r.status; out.data = r.data; out.odd = odd; out.slot = sy.t - 0.8 * kT * fs - pulse0_off_s(lv, true) * fs; return out; }
	}
	return out;
}

Packet decode(int level, bool lng, const std::vector<float> &x, double fs, double s, double search_ms)
{
	Packet out;
	if (level == 1) return lng ? sl1_long(x, fs, s, search_ms) : sl1_short(x, fs, s, search_ms);
	if (lng) return lv_long(level, x, fs, s, search_ms);
	const double base = s + 0.8 * kT * fs + pulse0_off_s(level, false) * fs;
	if (level == 5) {
		const p3::RxPacket r = p3::decode_at(x.data(), long(x.size()), fs, base, search_ms);
		if (!r.ok) return out;
		out.ok = true; out.level = 5; out.status = r.r.info[p3::kDataBytes]; out.data.assign(r.r.info, r.r.info + p3::kDataBytes); out.odd = r.odd; out.slot = r.t0 - 0.8 * kT * fs - pulse0_off_s(5, false) * fs;
		return out;
	}
	const p3::LvRx r = p3::decode_level(level, x.data(), long(x.size()), fs, base, search_ms);
	if (!r.ok) return out;
	out.ok = true; out.level = level; out.status = r.status; out.data = r.data; out.odd = r.odd; out.slot = r.t0 - 0.8 * kT * fs - pulse0_off_s(level, false) * fs;
	return out;
}

std::vector<uint8_t> words(int level, bool lng, const uint8_t *data, uint8_t status)
{
	const int counter = status & 3;
	if (lng) return p3::long_encode(level, data, status);
	if (level == 1) {
		// SL1 short: P2 level-1 words; the first (the reference pulse, its step only sets the
		// carriers' starting phase) is 0xaa in the firmware's unproto, P2's ARQ word 0x6a in ours
		// (U 30: every other word of all 56 packets identical)
		uint8_t info[8]; p3::sl1_make_info(data, status, info);
		auto w = p2::encode(1, info, counter);
		w[0] = 0xaa;
		return w;
	}
	if (level == 5) {
		uint8_t info[p3::kInfoBytes];
		std::memcpy(info, data, size_t(p3::kDataBytes));
		info[p3::kDataBytes] = status;
		const uint16_t c = crc16_ccitt_reflected(info, size_t(p3::kDataBytes + 1));
		info[p3::kDataBytes + 1] = uint8_t(c); info[p3::kDataBytes + 2] = uint8_t(c >> 8);
		return p3::encode(info, counter, false);
	}
	return p3::encode_level(level, data, status);
}

void shift(std::vector<float> &a, double hz, double fs)
{
	constexpr int N = 127;
	static const std::vector<double> h = [] {
		std::vector<double> v(N, 0.0);
		for (int k = 0; k < N; k++) { const int m = k - N / 2; v[size_t(k)] = (m % 2) ? 2.0 / (M_PI * m) * (0.54 - 0.46 * std::cos(2 * M_PI * k / (N - 1))) : 0.0; }
		return v;
	}();
	const long n = long(a.size());
	std::vector<float> out(a.size());
	for (long i = 0; i < n; i++) {
		double im = 0;
		for (int k = 0; k < N; k++) { const long j = i + N / 2 - k; if (j >= 0 && j < n) im += h[size_t(k)] * a[size_t(j)]; }
		const double ph = 2 * M_PI * hz * double(i) / fs;
		out[size_t(i)] = float(a[size_t(i)] * std::cos(ph) - im * std::sin(ph));
	}
	a.swap(out);
}

fsearch::Result offset(const std::vector<float> &x, double fs, int level)
{
	static const fsearch::PulsePower pw_flat(p3::kPulse9600, p3::kPulseHalf9600), pw_cs(p3::kCsPulse9600, p3::kCsPulseHalf9600);
	double car[18], wt[18]; int nc = 0; double range = 70;
	const fsearch::PulsePower *pw = &pw_cs;
	if (level == 1) { car[0] = p3::kCsLo; car[1] = p3::kCsHi; nc = 2; range = 100; }
	else if (level == 2) { const auto &L = p3::layout(2); for (int j = 0; j < L.sec[0].n; j++) car[nc++] = p3::carrier_hz(L.sec[0].even[j]); }
	else { for (int k = 0; k < 18; k++) car[nc++] = p3::carrier_hz(k); pw = &pw_flat; }
	for (int j = 0; j < nc; j++) wt[j] = 1.0;
	auto r = fsearch::burst_offset(x.data(), long(x.size()), fs, car, wt, nc, *pw, 0.0, range);
	if (r.match < 0.35) r.ok = false;
	return r;
}

std::vector<int> levels_for(const std::vector<float> &x, double fs, double a)
{
	double p[18] = {};
	const long n0 = long(a), n1 = std::min(long(x.size()), long(a + 0.5 * fs));
	if (n1 - n0 < long(0.3 * fs)) return {};
	for (int k = 0; k < 18; k++) {
		const double w = 2 * M_PI * (480.0 + 120.0 * k) / fs;
		std::complex<double> acc(0, 0), rot = std::polar(1.0, -w), ph = std::polar(1.0, -w * double(n0));
		// 50 ms pieces summed in power (a carrier's phase steps every pulse)
		const long blk = long(0.05 * fs);
		double pw = 0;
		for (long i = n0; i < n1; i++) { acc += double(x[size_t(i)]) * ph; ph *= rot; if ((i - n0 + 1) % blk == 0) { pw += std::norm(acc); acc = 0; ph /= std::abs(ph); } }
		p[k] = pw;
	}
	// the carriers on for the whole packet (SL4's and SL5's header carriers, k1/k16 and k0/k17, are on for
	// 9 pulses only; a radio's filter takes the edges down further)
	static const unsigned kSet[7] = {0, 1u << 5 | 1u << 12, 1u << 3 | 1u << 5 | 1u << 7 | 1u << 10 | 1u << 12 | 1u << 14, 0xFFFCu, 0xFFFCu, 0x1FFFEu, 0x1FFFEu};
	std::vector<std::pair<double, int>> fit;
	double mid = 0; for (int k = 2; k <= 15; k++) mid += p[k]; mid /= 14;
	const double edge = 0.5 * (p[0] + p[17]) / std::max(mid, 1e-30);   // SL6 ~1, SL5 ~0.11 (header), others 0
	const double edge1 = 0.5 * (p[1] + p[16]) / std::max(mid, 1e-30);  // SL5/6 ~1, SL4 ~0.11 (header), SL3 0
	for (int lv = 1; lv <= 6; lv++) {
		const unsigned set = kSet[lv];
		double in = 0, out = 0, lo = 1e300; int ni = 0, no = 0;
		for (int k = 0; k < 18; k++) {
			if (set >> k & 1) { in += p[k]; ni++; lo = std::min(lo, p[k]); }
			else if (lv < 5) { out += p[k]; no++; }      // SL5/6: k0 / k17 judged by the edge ratio below
		}
		in /= ni; out = no ? out / no : 0;
		// silence (a FIFO's zeros, a muted receiver) fits nothing: a carrier ~4 counts or more
		if (in < 1e6 * double(n1 - n0) / (0.05 * fs)) continue;
		// every carrier of the level on (a narrow signal, P2's 1400/1600 Hz or P1, fits no level
		// by its out-of-set power alone), and little power off them
		if (lo < 0.03 * in || out > 0.25 * in) continue;
		// SL5/6 carry data on k1 / k16, SL3/4 do not (on the air an SL4 burst through the radios was tried at SL5
		// and SL6 first, ~2 s of a Raspberry Pi's time): judged by those two carriers against the middle
		if ((lv >= 5 && edge1 < 0.4) || ((lv == 3 || lv == 4) && edge1 > 0.6)) continue;
		// SL5 / SL6 (and SL3 / SL4) look alike here: both tried, the one the edge carriers favour first
		const double pref = lv == 6 ? (edge >= 0.33 ? 0 : 1) : lv == 5 ? (edge >= 0.33 ? 1 : 0) : lv == 4 ? 0 : 0.5;
		fit.push_back({out / in + 0.1 * (1 - lo / in) + 1e-3 * pref, lv});   // cleanest and flattest fit first
	}
	std::sort(fit.begin(), fit.end());
	std::vector<int> r;
	for (const auto &f : fit) r.push_back(f.second);
	return r;
}

}  // namespace fmodem::p3u

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// The functions of p3_rx.hpp (out of the header, so that they build in parallel).
#include "p3_rx.hpp"

namespace fmodem::p3 {

LvSync sync_level(int level, const float *x, long n, double fs, double t0, double search_ms, double df)
{
	const Layout &L = layout(level);
	const bool flat = !L.narrow;
	auto resid_at = [&](double t, bool odd) {
		double r = 1;
		if (flat) demod_flat(level, x, n, fs, t, df, &r, 12);
		else demod_lv(level, x, n, fs, t, odd, df, &r, 12);
		return r;
	};
	LvSync s; s.t = t0;
	const double step = fs / 9600 * 2, span = search_ms * 1e-3 * fs;
	if (search_ms > 4.0) {
		// Fix 219: beyond +-4 ms (the receiver re-acquiring after misses) the coarse grid is coarser in
		// proportion and then refined twice (about the work of +-2 ms). Up to +-4 ms: as before.
		const double cs = step * std::floor(search_ms / 2.0);
		for (int odd = 0; odd < (flat ? 1 : 2); odd++) {
			for (double dt = -span; dt <= span + 1e-9; dt += cs) {
				const double r = resid_at(t0 + dt, odd);
				if (r < s.r) { s.r = r; s.t = t0 + dt; s.odd = odd; }
			}
			for (const double w : {cs, step}) {
				const double c = s.t;
				for (double dt = -w; dt <= w + 1e-9; dt += w / 4) {
					const double r = resid_at(c + dt, odd);
					if (r < s.r) { s.r = r; s.t = c + dt; s.odd = odd; }
				}
			}
		}
		return s;
	}
	for (int odd = 0; odd < (flat ? 1 : 2); odd++) {
		for (double dt = -span; dt <= span + 1e-9; dt += step) {
			const double r = resid_at(t0 + dt, odd);
			if (r < s.r) { s.r = r; s.t = t0 + dt; s.odd = odd; }
		}
		for (double dt = -step; dt <= step + 1e-9; dt += step / 4) {
			const double r = resid_at(s.t + dt, odd);
			if (r < s.r) { s.r = r; s.t = s.t + dt; s.odd = odd; }
		}
	}
	return s;
}

LvRx finish_level(int level, const float *x, long n, double fs, const LvSync &s, double df,
                         std::vector<std::complex<double>> *dd0, std::vector<std::complex<double>> *dd1,
                         const FlatPlan *plan)
{
	LvRx out; out.level = level;
	const Layout &L = layout(level);
	const bool flat = !L.narrow;
	const double best_t = s.t; const bool best_odd = s.odd;
	out.t0 = best_t; out.resid = s.r;
	std::vector<double> st(size_t(L.nwords()), 0.0), wt(size_t(L.nwords()), 0.0);
	// soft bits weighted by each word's |differential product| (normalised;
	// 2026-10-18: SL3 at noise 2800 4 -> 21 of 60 single copies). P3_NO_WT=1: off.
	static const bool use_wt = std::getenv("P3_NO_WT") == nullptr;
	auto norm_wt = [&]() { double m = 0; int c = 0; for (double v : wt) if (v > 0) { m += v; c++; } if (c) for (double &v : wt) v /= (m / c); };
	std::vector<std::array<std::complex<double>, 18>> zf;
	// a plan made while the packet arrived (fix 152), if it is for this level, timing and offset
	const bool planned = plan && plan->ok && plan->level == level && plan->t0 == best_t && plan->df == df && plan->fs == fs && plan->p1 < 0 && !plan->lng;
	if (flat) zf = planned ? demod_flat_run(*plan, x, n) : demod_flat(level, x, n, fs, best_t, df);
	// memory ARQ: this copy's differential products for both maps, from the
	// same demodulation (no second solve; 2026-10-18: a second one doubled
	// the decode time of failed packets on the air)
	if (dd0 && dd1) {
		dd0->assign(size_t(L.nwords()), {0, 0}); dd1->assign(size_t(L.nwords()), {0, 0});
		if (flat) {
			for (int p = 1; p < L.npulses(); p++)
				for (int j = 0; j < L.carriers(p); j++)
					for (int odd = 0; odd < 2; odd++) {
						const int k = L.carrier(p, j, odd);
						(odd ? *dd1 : *dd0)[size_t(L.word_index(p, j))] = zf[size_t(p)][size_t(k)] * std::conj(zf[size_t(p - 1)][size_t(k)]);
					}
		}
	}
	for (int odd = 0; odd < 2; odd++) {
		if (!flat && odd != best_odd) continue;
		if (flat) {
			for (int p = 1; p < L.npulses(); p++)
				for (int j = 0; j < L.carriers(p); j++) {
					const int k = L.carrier(p, j, odd);
					const std::complex<double> pr = zf[size_t(p)][size_t(k)] * std::conj(zf[size_t(p - 1)][size_t(k)]);
					double s2 = -std::arg(pr) / (2 * M_PI) * 16;
					st[size_t(L.word_index(p, j))] = s2 < 0 ? s2 + 16 : s2;
					wt[size_t(L.word_index(p, j))] = std::abs(pr);
				}
		} else {
			auto z = demod_lv(level, x, n, fs, best_t, odd, df);
			for (int p = 1; p < L.npulses(); p++)
				for (int j = 0; j < L.carriers(p); j++) {
					st[size_t(L.word_index(p, j))] = lv_step(z, L, p, j, odd);
					const int k = L.carrier(p, j, odd);
					int jp = -1;
					for (int q = 0; q < L.carriers(p - 1); q++) if (L.carrier(p - 1, q, odd) == k) jp = q;
					wt[size_t(L.word_index(p, j))] = jp >= 0 ? std::abs(z[size_t(p)][size_t(j)]) * std::abs(z[size_t(p - 1)][size_t(jp)]) : 0.0;
				}
			if (dd0 && dd1) {                          // narrow (SL2): this map from z, the other one demodulated
				for (int om = 0; om < 2; om++) {
					const bool m = om ? !odd : odd;
					const auto zz = om ? demod_lv(level, x, n, fs, best_t, m, df) : z;
					auto &d = m ? *dd1 : *dd0;
					for (int p = 1; p < L.npulses(); p++)
						for (int j = 0; j < L.carriers(p); j++) {
							const int k = L.carrier(p, j, m);
							int jp = -1;
							for (int q = 0; q < L.carriers(p - 1); q++) if (L.carrier(p - 1, q, m) == k) jp = q;
							if (jp >= 0) d[size_t(L.word_index(p, j))] = zz[size_t(p)][size_t(j)] * std::conj(zz[size_t(p - 1)][size_t(jp)]);
						}
				}
			}
		}
		norm_wt();
		const double *w = use_wt ? wt.data() : nullptr;
		const Lv23Rx r = level == 6 ? sl6_decode(st.data(), w) : level == 4 ? lv4_decode(st.data(), w) : lv23_decode(level, st.data(), -1, w);
		if (r.ok) {
			out.ok = true; out.odd = odd; out.data = r.data; out.status = r.status; out.agree = r.agree;
			// the residual frequency offset, from the steps against the words that
			// were sent (re-encoded from the decoded data; no decision ambiguity)
			const auto w = encode_level(level, r.data.data(), r.status);
			std::complex<double> acc = 0;
			for (size_t j = 0; j < w.size() && j < st.size(); j++) acc += std::polar(1.0, 2 * M_PI * (st[j] - double(w[j])) / 16.0);
			out.step_bias = 16.0 * std::arg(acc) / (2 * M_PI);
			return out;
		}
	}
	return out;
}

void level_diffs(int level, const float *x, long n, double fs, const LvSync &s, double df,
                        std::vector<std::complex<double>> &d0, std::vector<std::complex<double>> &d1)
{
	const Layout &L = layout(level);
	d0.assign(size_t(L.nwords()), {0, 0}); d1.assign(size_t(L.nwords()), {0, 0});
	if (!L.narrow) {
		const auto zf = demod_flat(level, x, n, fs, s.t, df);
		for (int p = 1; p < L.npulses(); p++)
			for (int j = 0; j < L.carriers(p); j++)
				for (int odd = 0; odd < 2; odd++) {
					const int k = L.carrier(p, j, odd);
					(odd ? d1 : d0)[size_t(L.word_index(p, j))] = zf[size_t(p)][size_t(k)] * std::conj(zf[size_t(p - 1)][size_t(k)]);
				}
		return;
	}
	for (int odd = 0; odd < 2; odd++) {
		const auto z = demod_lv(level, x, n, fs, s.t, odd, df);
		auto &d = odd ? d1 : d0;
		for (int p = 1; p < L.npulses(); p++)
			for (int j = 0; j < L.carriers(p); j++) {
				const int k = L.carrier(p, j, odd);
				int jp = -1;
				for (int q = 0; q < L.carriers(p - 1); q++) if (L.carrier(p - 1, q, odd) == k) jp = q;
				if (jp >= 0) d[size_t(L.word_index(p, j))] = z[size_t(p)][size_t(j)] * std::conj(z[size_t(p - 1)][size_t(jp)]);
			}
	}
}

Lv23Rx decode_level_diffs(int level, const std::vector<std::complex<double>> &d)
{
	static const bool use_wt = std::getenv("P3_NO_WT") == nullptr;
	std::vector<double> st(d.size(), 0.0), wt(d.size(), 1.0);
	double m = 0; int c = 0;
	for (size_t i = 0; i < d.size(); i++) {
		const double v = -std::arg(d[i]) / (2 * M_PI) * 16; st[i] = v < 0 ? v + 16 : v;
		if (std::abs(d[i]) > 0) { m += std::abs(d[i]); c++; }
	}
	if (c) for (size_t i = 0; i < d.size(); i++) wt[i] = std::abs(d[i]) / (m / c);
	const double *w = use_wt && c ? wt.data() : nullptr;
	return level == 6 ? sl6_decode(st.data(), w) : level == 4 ? lv4_decode(st.data(), w) : lv23_decode(level, st.data(), -1, w);
}

}  // namespace fmodem::p3

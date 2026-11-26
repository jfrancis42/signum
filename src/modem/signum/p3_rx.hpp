// P3 receive for speed levels 2-4 and 6 (SL5: p3_modem.hpp decode_at; SL1:
// the P2 two-carrier receiver). Timing search on the first pulses by LS
// residual, then a full demodulation and decode with both carrier maps.
#pragma once

#include <array>
#include <complex>
#include <vector>

#include "p3_levels.hpp"
#include "p3_lv23.hpp"
#include "p3_sl6.hpp"

namespace fmodem::p3 {

struct LvRx { bool ok = false; int level = 0; bool odd = false; double t0 = 0, resid = 1, agree = 1; std::vector<uint8_t> data; uint8_t status = 0;
	double step_bias = 0; };   // decoded: the mean step error against the re-encoded words (1/16 turn per pulse), i.e. the residual offset

// Pulse-0 centre relative to the slot's word-0 centre (P2 convention:
// slot + 0.8 T), measured on the emulated firmware's ARQ audio.
inline double level_offset_s(int level)
{
	switch (level) { case 2: return 0.0; case 3: return 23.75 / 9600.0; case 4: return 24.0 / 9600.0; default: return 25.5 / 9600.0; }
}

// Timing (and carrier map) from the first 12 pulses: needs only the start of
// the packet, so a real-time receiver can run it while the rest arrives.
struct LvSync { double t = 0, r = 9; bool odd = false; };
LvSync sync_level(int level, const float *x, long n, double fs, double t0, double search_ms, double df = 0);

inline std::vector<uint8_t> encode_level(int level, const uint8_t *data, uint8_t status);
// Demodulate the whole packet at the sync_level() timing and decode.
// df: the carrier frequency offset (Hz) to remove, tracked by the caller
// (P3 link: from the SL5 headers).
LvRx finish_level(int level, const float *x, long n, double fs, const LvSync &s, double df = 0,
                         std::vector<std::complex<double>> *dd0 = nullptr, std::vector<std::complex<double>> *dd1 = nullptr,
                         const FlatPlan *plan = nullptr);

// Memory ARQ (2026-10-18): one copy's differential products per word for
// both carrier maps (a failed copy's map is unknown), and a decode from
// (summed) products.
void level_diffs(int level, const float *x, long n, double fs, const LvSync &s, double df,
                        std::vector<std::complex<double>> &d0, std::vector<std::complex<double>> &d1);
Lv23Rx decode_level_diffs(int level, const std::vector<std::complex<double>> &d);

inline LvRx decode_level(int level, const float *x, long n, double fs, double t0, double search_ms, double df = 0)
{
	return finish_level(level, x, n, fs, sync_level(level, x, n, fs, t0, search_ms, df), df);
}

inline std::vector<uint8_t> encode_level(int level, const uint8_t *data, uint8_t status)
{
	return level == 6 ? sl6_encode(data, status) : level == 4 ? lv4_encode(data, status) : lv23_encode(level, data, status);
}
inline int level_data_bytes(int level) { return level == 2 ? 23 : level == 3 ? 59 : level == 4 ? 122 : level == 5 ? 212 : level == 6 ? 284 : 5; }

}  // namespace fmodem::p3

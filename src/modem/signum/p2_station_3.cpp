// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// P2Station member functions, part 3 of 8 (split out of p2_station.hpp so they build in parallel).
#include "p2_station.hpp"

namespace fmodem {

void P2Station::trk_update(const RxPacket &q_in, int64_t exp)
{
	if (!q_in.ok || !q_in.has_c || q_in.bk || q_in.swap < 0) return;
	int64_t sq = trk_seq_of(exp, false);
	if (sq <= 0) sq = trk_n_;                         // a slot no look counted: the latest
	if (trk_ok_ && sq < trk_seq_) return;             // fix 200: an older slot's (late) decode after a newer one taught the track
	RxPacket q = q_in;
	double cr[2];
	if (q_in.has_rc ? (cr[0] = q_in.rc[0], cr[1] = q_in.rc[1], true) : trk_refine(q_in, cr)) {
		const int e = q.c[0] <= q.c[1] ? 0 : 1;
		q.start += cr[e] - q.c[e];
		q.c[0] = cr[0]; q.c[1] = cr[1];
	}
	const double dt = q.start - double(exp), stag = std::fabs(q.c[1] - q.c[0]);
	double lead = std::min(q.c[0], q.c[1]) - q.start;
	// Fix 191: the lead is a pulse grid's phase: a decode whose first pulse was
	// found a pulse early or late (word 0 then a pulse further along, lead
	// -19 or 173 samples for 77) is the same timing. Averaged with the track's
	// as it was, the track's lead sat half a pulse off for many packets, or
	// the demodulation kept starting a pulse late (its last pulses cut by the
	// window): P2 L1 -10 dB +20 Hz seed 1, 81 of 210 good packets. Taken
	// modulo the pulse to the nominal lead (77 samples at 9600 Hz, the
	// demodulator's word 0 at index 1). P2_TRK_LEAD_RAW: as before (test).
	static const bool lead_raw = std::getenv("P2_TRK_LEAD_RAW") != nullptr;
	if (!lead_raw) { const double l0 = nw_ / 2.0; lead -= T_ * std::round((lead - l0) / T_); }
	if (trk_ok_ && sq - trk_seq_ == 1) trk_alt_ = q.swap != trk_swap_;   // consecutive transmissions
	if (!trk_ok_ || std::fabs(dt - trk_dt_) > 0.6 * T_ || std::fabs(stag - trk_stag_) > 0.3 * T_) { trk_dt_ = dt; trk_lead_ = lead; trk_stag_ = stag; }
	else { trk_dt_ += 0.3 * (dt - trk_dt_); trk_lead_ += 0.3 * (lead - trk_lead_); trk_stag_ += 0.3 * (stag - trk_stag_); }
	trk_swap_ = q.swap; trk_seq_ = sq; trk_ok_ = true;
	if (std::getenv("TRK_EACH")) std::fprintf(stderr, "trk update dt %.2f lead %.2f stag %.2f (packet dt %.2f lead %.2f stag %.2f)\n", trk_dt_, trk_lead_, trk_stag_, dt, lead, stag);
}

void P2Station::trk_reframe(int64_t moved, int64_t exp)
{
	if (p3_ || !trk_ok_ || trk_frame_old()) return;
	const double cyc = double(samples(cyc_s()));
	if (trk_v_exp_ < 0) { trk_v_exp_ = exp; trk_v_n_ = 0; trk_v_i_ = 0; }
	const double k = std::round(double(exp - trk_v_exp_) / cyc);
	trk_v_k_[trk_v_i_] = k; trk_v_y_[trk_v_i_] = double(exp - trk_v_exp_) + trk_dt_ - k * cyc;
	trk_v_i_ = (trk_v_i_ + 1) % kTrkVPts; if (trk_v_n_ < kTrkVPts) trk_v_n_++;
	trk_dt_ -= double(moved);
	double kmin = 1e18, kmax = -1e18;
	for (int i = 0; i < trk_v_n_; i++) { kmin = std::min(kmin, trk_v_k_[i]); kmax = std::max(kmax, trk_v_k_[i]); }
	if (trk_v_n_ >= kTrkVMin && kmax - kmin >= kTrkVSpan) {
		double sl[kTrkVPts * (kTrkVPts - 1) / 2]; int m = 0;
		for (int i = 0; i < trk_v_n_; i++)
			for (int j = i + 1; j < trk_v_n_; j++)
				if (trk_v_k_[j] != trk_v_k_[i]) sl[m++] = (trk_v_y_[j] - trk_v_y_[i]) / (trk_v_k_[j] - trk_v_k_[i]);
		if (m > 0) { std::nth_element(sl, sl + m / 2, sl + m); trk_v_ = std::max(-kTrkVMax, std::min(kTrkVMax, sl[m / 2])); }
	}
	if (std::getenv("TRK_EACH")) std::fprintf(stderr, "trk reframe %lld samples: dt now %.2f, drift %.3f a cycle\n", (long long)moved, trk_dt_, trk_v_);
}

auto P2Station::marq(RxPacket q, int64_t exp) -> RxPacket
{
	led_marq_ = 0;                                    // the panel shows this packet's result (one cycle)
	if (q.ok) { marq_clear(); if (q.swap >= 0) marq_swap_ = q.swap; trk_update(q, exp); return q; }
	if (marq_on() && debug_) log("MARQ: copy llr " + std::to_string(q.llr.size()) + " level " + std::to_string(q.llr_level) + " parity " + std::to_string(q.llr_parity) +
	                           " rms " + std::to_string(q.llr_rms) + " (sum: " + std::to_string(marq_n_) + " copies, level " + std::to_string(marq_level_) + " parity " + std::to_string(marq_parity_) + ")");
	if (!marq_on() || q.llr.empty() || q.bk) return q;
	const float w = marq_weight(q);
	// Consecutive failures are copies of one packet (the sender moves on
	// only after our acknowledgement); the noisy header's parity is not
	// trusted. A sum that has not decoded after kMarqRestart copies
	// starts again (a copy from a wrong alignment or level spoils it).
	if (marq_n_ > 0 && q.llr_level == marq_level_ && marq_sum_.size() == q.llr.size() && marq_n_ < kMarqRestart) {
		const bool pre = q.mq_pre && q.mq_ver == marq_ver_ && !std::getenv("P2_MARQ_INLINE");   // decoded ahead on the worker (2026-10-19: 5-9 ms on the audio loop at L3/L4)
		for (size_t i = 0; i < marq_sum_.size(); i++) marq_sum_[i] += w * q.llr[i];
		marq_n_++; marq_ver_++;
		p2::RxResult res;
		if (pre) { res.ok = q.mq_pre_ok; res.info = q.mq_info; res.metric = q.mq_metric; }
		else res = p2::decode_llr(marq_level_, marq_sum_, llr_ppr(marq_level_, marq_sum_.size()));
		std::vector<uint8_t> inf = res.info;
		if (p3_ ? (marq_level_ == 1 && sl1_plain(inf)) : res.ok) {
			RxPacket r = q;
			r.ok = true; r.level = marq_level_; r.info = inf; r.start = double(exp); r.llr.clear(); r.metric = res.metric; r.marq_n = marq_n_;
			log("MARQ: decoded from " + std::to_string(marq_n_) + " copies");
			marq_clear();
			led_marq_ = 1;                            // MARQ-OK (after the clear, which darkens it)
			return r;
		}
		led_marq_ = 2;
		return q;
	}
	marq_sum_.assign(q.llr.size(), 0.0f);
	for (size_t i = 0; i < marq_sum_.size(); i++) marq_sum_[i] = w * q.llr[i];
	marq_level_ = q.llr_level; marq_parity_ = q.llr_parity; marq_n_ = 1; marq_ver_++;
	return q;
}

uint8_t P2Station::more_bit(bool more, size_t queued, int cap) const
{
	if (!p3_) return more ? 0x20 : 0;
	static const bool old = std::getenv("P3_MORE_OLD") != nullptr;
	static const bool nohold = std::getenv("P3_MORE_HOLD") == nullptr;
	// Long packets (and SL1): 0x20 always, as before (the reference unit's flag up: it keeps us on
	// long packets; its own sender lets it fall as its text runs low, and its receiver then asks
	// R back to short. Kept long, p3-send-fade
	// (CCIR moderate + noise) printed 3,525; following the reference unit, 2,656.)
	// P3_LONG_MORE_LOW=1 (test): on long packets the flag down under 5/3 of a long packet queued,
	// so a test sender ends its text as the reference unit does (its receiver
	// then asks R back to short packets).
	static const bool long_low = std::getenv("P3_LONG_MORE_LOW") != nullptr;
	if (long_low && !old && p3_long_ && p3_level_ >= 2) return uint8_t(queued * 3 >= size_t(5 * cap) ? 0x20 : 0);
	if (old || p3_long_ || p3_level_ < 2) return 0x20;
	bool lots = queued * (p3_long_ ? 3 : 1) >= size_t(5 * cap);
	if (!nohold && !p3_long_ && p3_level_ < 6 && !last_pkt_full_) lots = false;
	// Not with a frequency offset: at 50 Hz the reference firmware asked R on the flag and then
	// decoded none of our SL6 long packets (R R R ..., p3-off50-it-call-we-send: 1,435
	// characters, 4,167 on short packets); its own pair links there at all only short (216).
	// Fix 217b: 5 Hz (was 20): on the reference unit at 10 Hz the firmware failed our SL6 long packets
	// (R R R, 1,432 characters against 6,788 at 0 Hz), and the real reference unit at -19 Hz (f217-8).
	// Fix 223: the limit is on the offset the reference unit hears us at: ours less our transmit shift (as
	// caller we follow it, so ~0 at any offset up to the 50 Hz clamp; as callee the whole offset,
	// as before). P3_LONG_GATE_ABS=1: on our own offset (217b).
	static const double max_df = std::getenv("P3_LONG_MAX_DF_TX") ? std::atof(std::getenv("P3_LONG_MAX_DF_TX")) : kP3LongMaxDfTx;
	static const bool gate_abs = std::getenv("P3_LONG_GATE_ABS") != nullptr;
	// Only as caller: a reference unit caller moves its own transmit toward us (fix 223), so as callee
	// fix 145's shift (from 40 Hz) does not put us on its frequency: p3-off50-it-call-we-send fell
	// 4,167 -> 1,435 with the shift counted there (its R, then none of our long packets decoded).
	const double u = gate_abs || !master_ ? 0.0 : tx_shift_now();
	const double heard = std::max(std::fabs(afc_hz() - u), p3_df_set_ || std::fabs(p3_rx_df_) > 0 ? std::fabs(-p3_rx_df_ - u) : 0.0);
	if (!p3_long_ && heard > max_df) lots = false;
	// Fix 225b: long packets in real time again (fix 225's placement proven on the real reference unit,
	// f225-1..3: SL6 long packets taken first time, 12,000 in 41-45 s against 59 s short-only).
	// P3_LONG_TX_RT=0 restores fix 225's stop-gap (no "much to send" in real time, so no R).
	static const bool long_tx_rt = !(std::getenv("P3_LONG_TX_RT") && std::atoi(std::getenv("P3_LONG_TX_RT")) == 0);
	if (!p3_long_ && cfg_.realtime && !long_tx_rt) lots = false;
	return uint8_t(lots != p3_short_xor(p3_level_, p3_long_) ? 0x20 : 0);
}

void P2Station::build_packet(bool new_info)
{
	if (new_info) {
		txq_.erase(0, size_t(pending_used_));
		counter_ = (counter_ + 1) & 3;
		const int cap = capacity();
		const int type = fill_pending(cap);
		const bool more = txq_.size() > size_t(pending_used_);
		const bool over = over_due(pending_used_);
		if (over) sent_text_ = false;
		if (pending_used_ > 0) sent_text_ = true;
		pending_qrt_ = want_qrt_ && txq_.empty();     // QRT: status bit 7, idle data
		if (pending_qrt_) qrt_tx_since_take_ = true;
		pending_status_ = uint8_t(counter_ | type << 2 | more_bit(more, txq_.size(), cap) | (over ? 0x40 : 0) | (pending_qrt_ ? 0x80 : 0));
		last_pkt_full_ = more;                        // fix 217: this packet is full (text left beyond it)
		// front panel, sender: what the new packet holds
		led_te_ = pending_used_ > 0 ? 1 : 0;
		led_ir_ = pending_used_ < cap ? 1 : 0;
		led_tx_type_ = pending_used_ > 0 ? type : 0;
	}
	transmit_pending();
}

int P2Station::fill_pending(int cap)
{
	pending_data_.assign(size_t(cap), 0x1e);
	// Long packets carry the whole status byte through the coding (as short
	// ones do), so they may be compressed too.
	int type = cfg_.compress ? text::choose(txq_, cap, cfg_.allow_pmc, !p3_) : 0;
	if (const char *ft = std::getenv("P3_FORCE_TYPE")) type = std::atoi(ft);   // test: one data type for every packet
	if (const char *fs = std::getenv("P3_FORCE_SEQ"); fs && cfg_.compress && !txq_.empty()) {   // test: the types of the first data packets ("6,4,4")
		static int seq_i = 0; static int64_t seq_last = -1;
		if (seq_last != counter_ + 1000 * int64_t(level_)) { seq_last = counter_ + 1000 * int64_t(level_); }
		int k = 0; const char *q = fs;
		while (*q && k < seq_i) { if (*q == ',') k++; q++; }
		if (*q) type = std::atoi(q);
		seq_i++;
	}
	// test hook (fix 220): with P2_TEST_ZERO_PKT set, each '~' in the text is sent as one packet of data
	// type 1 (Huffman) whose information is all zero bits, as the reference unit's junk packets
	if (static const bool zp = std::getenv("P2_TEST_ZERO_PKT") != nullptr; zp && !txq_.empty()) {
		if (txq_[0] == '~') { pending_data_.assign(size_t(cap), 0); pending_used_ = 1; return 1; }
		const size_t t = txq_.find('~');
		if (t != std::string::npos) {
			pending_used_ = text::encode(type, txq_.substr(0, t), pending_data_.data(), cap, !p3_);
			return type;
		}
	}
	pending_used_ = text::encode(type, txq_, pending_data_.data(), cap, !p3_);
	// This packet drained the queue: close a held run now, for the next
	// packet (the firmware packs, then flushes).
	if (size_t(pending_used_) >= txq_.size()) txq_ += rle_tx_.flush();
	return type;
}

int P2Station::tx_payload() const
{
	if (p3_ && p3_long_) return p3::long_payload(p3_level_);
	if (p3_ && p3_level_ == 1) return p3::kSl1Data;
	if (p3_ && ((p3_level_ >= 2 && p3_level_ <= 4) || p3_level_ == 6)) return p3::level_data_bytes(p3_level_);
	if (p3_) return p3::kDataBytes;
	return p2::data_bytes(level_, long_ ? 40 : 9);
}

auto P2Station::tx_words(const std::vector<uint8_t> &data, uint8_t st, int counter, bool first) const -> TxWords
{
	const int hdr = counter ^ hdr_flip_;
	// P3 level packets: their header template follows the status parity; when
	// the header toggle differs, the header pulses (the first hdr_words) come
	// from an encoding with the other parity.
	auto with_hdr = [&](auto enc, int hdr_words) {
		auto w = enc(st);
		if (((st ^ hdr) & 1) == 0) return w;
		const auto alt = enc(uint8_t(st ^ 1));
		std::copy(alt.begin(), alt.begin() + std::min<ptrdiff_t>(hdr_words, ptrdiff_t(alt.size())), w.begin());
		return w;
	};
	if (p3_ && p3_long_) {                          // long-cycle packet (after the receiver's R)
		auto w = with_hdr([&](uint8_t s) { return p3::long_encode(p3_level_, data.data(), s); }, p3_level_ == 1 ? 9 : 9 * kHdrWords[p3_level_]);
		return {p3_level_ == 1 ? TX_PSK : TX_LV, p3_level_, true, std::move(w)};
	}
	if (p3_ && p3_level_ == 1) {
		uint8_t info[8];
		p3::sl1_make_info(data.data(), st, info);
		return {TX_PSK, 1, false, p2::encode(1, info, hdr)};
	}
	if (p3_ && ((p3_level_ >= 2 && p3_level_ <= 4) || p3_level_ == 6))
		return {TX_LV, p3_level_, false, with_hdr([&](uint8_t s) { return p3::encode_level(p3_level_, data.data(), s); }, 9 * kHdrWords[p3_level_])};
	if (p3_) {
		uint8_t info[p3::kInfoBytes];
		std::memcpy(info, data.data(), size_t(p3::kDataBytes));
		info[p3::kDataBytes] = st;
		const uint16_t c = crc16_ccitt_reflected(info, size_t(p3::kDataBytes + 1));
		info[p3::kDataBytes + 1] = uint8_t(c); info[p3::kDataBytes + 2] = uint8_t(c >> 8);
		return {TX_P3, 5, false, p3::encode(info, hdr, first)};
	}
	const int ppr = long_ ? 40 : 9;
	const int n = p2::data_bytes(level_, ppr);
	std::vector<uint8_t> info(size_t(n + 3));
	p2::make_info(level_, data.data(), st, info.data(), ppr);
	std::vector<uint8_t> w;
	if (long_) {
		w.push_back(0x6a);
		const auto &h = p2arq::kLongHeader[level_][hdr & 1];
		w.insert(w.end(), h, h + 8);
		const auto d = p2::encode_data(level_, info.data(), ppr);
		w.insert(w.end(), d.begin(), d.end());
		w.push_back(p2arq::kLongTail[level_]);
	} else {
		w = p2::encode(level_, info.data(), hdr);
	}
	return {TX_PSK, level_, long_, std::move(w)};
}

void P2Station::transmit_pending()
{
	if (qrt_end_sent_) {                           // repeat the QRT end packet
		queue_psk(next_start_, qrt_end_words(counter_));
		my_start_ = next_start_;
		schedule(reply_check_at(my_start_, false), EV_REPLY, my_start_, 0);
		return;
	}
	if (!p3_ && long_ && !long_ok(level_)) long_ = false;
	pending_data_.resize(size_t(tx_payload()), 0x1e);
	const TxWords t = tx_words(pending_data_, pending_status_, counter_, p3_first_tx_);
	premod_hit_ = false; premod_why_ = nullptr;
	if (t.kind == TX_LV) queue_lv(next_start_, t.level, t.w, t.lng);
	else if (t.kind == TX_P3) queue_p3(next_start_, t.w);
	else queue_psk(next_start_, t.w);
	if (p3_) p3_first_tx_ = false;
	if (premod_why_ && cfg_.realtime)              // fix 273: on air, the answer's change found its packet ready
		log("P3: SL" + std::to_string(p3_level_) + (p3_long_ ? " long" : "") + " packet made ahead for the change (" + premod_why_ + ", fix 273)");
	my_start_ = next_start_;
	const bool lng = p3_ ? p3_long_ : long_;
	schedule(reply_check_at(my_start_, lng), EV_REPLY, my_start_, lng ? 1 : 0);
	if (debug_ && std::getenv("P2_TX_HEX")) {   // test: the packet's data bytes
		std::string h;
		char b[4];
		for (uint8_t v : pending_data_) { std::snprintf(b, sizeof b, "%02x", v); h += b; }
		log("tx data " + h);
	}
	if (debug_) {
		const std::string ahead = premod_hit_ ? " (made ahead)" : "";
		if (p3_ && p3_long_) log("send P3 SL" + std::to_string(p3_level_) + " LONG status " + std::to_string(pending_status_) + " used " + std::to_string(pending_used_) + ahead);
		else if (p3_ && p3_level_ != 5) log("send P3 SL" + std::to_string(p3_level_) + " status " + std::to_string(pending_status_) + " used " + std::to_string(pending_used_) + ahead);
		else if (p3_) log("send P3 status " + std::to_string(pending_status_) + " used " + std::to_string(pending_used_) + ahead);
		else log("send L" + std::to_string(level_) + (long_ ? " long" : "") + " status " + std::to_string(pending_status_) + " used " + std::to_string(pending_used_) + ahead);
	}
	premod_next();
}

std::shared_future<std::vector<float>> P2Station::premod_take(const PremodKey &k, std::shared_ptr<TxTrace> *tr, std::shared_ptr<TxProg> *prog)
{
	std::shared_future<std::vector<float>> f;
	if (premod_.empty()) return f;
	bool hit = false;
	for (auto &e : premod_)
		if (e.key == k) {
			if (e.prog && !prog) break;                // (never: a pieces key is taken by queue_psk)
			f = e.fut; hit = true; if (tr) *tr = e.tr; if (prog) *prog = e.prog; premod_why_ = e.why; break;
		}
	premod_.clear();
	if (hit) { premod_hit_ = true; premod_hits_++; } else premod_misses_++;
	return f;
}

std::vector<float> P2Station::premod_make(const PremodKey &k, float fs, float tone_lo, float tone_hi)
{
	const double lead = k.lead;
	if (k.kind == TX_P3) return mod_p3(k.w, fs, k.sw, k.amp, lead, k.clip);
	if (k.kind == TX_LV) return mod_lv(k.level, k.w, fs, k.sw, k.amp, lead, k.lng);
	if (k.kind == TX_CS) return p3::modulate_cs(k.w, fs, k.sw, k.amp);
	auto a = p2::modulate(k.w, fs, tone_lo, tone_hi, k.sw, k.amp, 0.0f);
	if (TxProg::test_us() > 0 && a.size() > 20000) std::this_thread::sleep_for(std::chrono::microseconds(int64_t(TxProg::test_us() * double(a.size()) / 256.0)));
	if (k.shift != 0.0) shift_audio(a, k.shift, fs);
	return a;
}

bool P2Station::premod_key(const TxWords &t, PremodKey &k) const
{
	k = PremodKey{t.kind, t.level, t.lng, psk_swap_, 0.0f, 0.0, t.w};
	if (t.kind == TX_P3 || t.kind == TX_LV) {
		// the lead queue_p3 / queue_lv will use (the same for every slot)
		int64_t st; double ld;
		p3_place(p3_pulse0(0, t.kind == TX_P3 ? 5 : t.level, t.lng), t.kind == TX_P3 ? 0.02 : 0.03, st, ld);
		k.lead = ld;
		k.amp = t.kind == TX_P3 ? kP3DataGain * p3_data_amp() : lv_gain(t.level) * p3_data_amp();
		if (t.kind == TX_P3) k.clip = p3_clip_now();
	}
	else {
		if (t.w.size() <= 100) return false;   // short PSK bursts are made on the audio loop
		k = psk_key(t.w, !p3_ && cfg_.tx_afc && std::fabs(afc_hz()) > 0.5 ? afc_hz() : 0.0);
	}
	return true;
}

bool P2Station::predict_new(std::vector<uint8_t> &d, uint8_t &st) const
{
	if (std::getenv("P3_FORCE_SEQ") || qrt_end_sent_) return false;
	std::string q = txq_.substr(std::min(txq_.size(), size_t(std::max(0, pending_used_))));
	const int ctr = (counter_ + 1) & 3;
	const int cap = capacity();
	d.assign(size_t(cap), 0x1e);
	int type = cfg_.compress ? text::choose(q, cap, cfg_.allow_pmc, !p3_) : 0;
	if (const char *ft = std::getenv("P3_FORCE_TYPE")) type = std::atoi(ft);
	const int used = text::encode(type, q, d.data(), cap, !p3_);
	if (size_t(used) >= q.size()) { text::RleCompressor r = rle_tx_; q += r.flush(); }
	const bool more = q.size() > size_t(used);
	const bool over = over_due(used);
	const bool qrt = want_qrt_ && q.empty();
	st = uint8_t(ctr | type << 2 | more_bit(more, q.size(), cap) | (over ? 0x40 : 0) | (qrt ? 0x80 : 0));
	d.resize(size_t(tx_payload()), 0x1e);
	return true;
}

void P2Station::premod_next(bool fresh)
{
	std::vector<Premod> old;
	old.swap(premod_);
	if (!premod_on() || !(cfg_.realtime || premod_check())) return;
	if (cfg_.realtime && pool_.size() == 0) return;   // no workers (small targets): it would run here, on the audio loop
	auto add = [&](const TxWords &t, const char *why = nullptr) {
		PremodKey k;
		if (!premod_key(t, k)) return;
		for (const auto &e : premod_) if (e.key == k) return;
		for (auto &e : old) if (e.key == k) { e.why = why; premod_.push_back(std::move(e)); return; }
		const float fs = cfg_.fs, lo = cfg_.tone_lo, hi = cfg_.tone_hi;
		if (!cfg_.realtime) { premod_.push_back(Premod{k, {}, {}, {}, why}); return; }
		auto tr = std::make_shared<TxTrace>();
		if (pieces_key(k)) { auto pr = prog_new(k); submit_prog(pr, tr, false); premod_.push_back(Premod{k, {}, tr, pr, why}); return; }
		premod_.push_back(Premod{k, pool_.submit(traced(tr, [k, fs, lo, hi]() { return premod_make(k, fs, lo, hi); })).share(), tr, {}, why});
	};
	add(tx_words(pending_data_, pending_status_, counter_, false));   // the repeat
	std::vector<uint8_t> d; uint8_t st = 0;
	if (predict_new(d, st)) add(tx_words(d, st, (counter_ + 1) & 3, false));
	if (p3_ && cfg_.realtime && !p3_tx_early_old()) {
		// Text arriving while we send (a typist: one call per key) would remake up to four bursts each
		// time: made again at most every kChgRedoS, the guesses made before kept until then.
		if (fresh || n_ - chg_at_ >= samples(kChgRedoS)) { premod_p3_changes(add); chg_at_ = n_; chg_redo_ = false; }
		else {
			for (auto &e : old) if (e.why && (e.fut.valid() || e.prog)) { bool dup = false; for (const auto &f : premod_) dup = dup || f.key == e.key; if (!dup) premod_.push_back(std::move(e)); }
			chg_redo_ = true;
		}
	}
}

bool P2Station::predict_same(std::vector<uint8_t> &d, uint8_t &st) const
{
	if (std::getenv("P3_FORCE_SEQ") || std::getenv("P2_TEST_ZERO_PKT") || qrt_end_sent_) return false;
	std::string q = txq_;
	const int cap = capacity();
	d.assign(size_t(cap), 0x1e);
	int type = cfg_.compress ? text::choose(q, cap, cfg_.allow_pmc, !p3_) : 0;
	if (const char *ft = std::getenv("P3_FORCE_TYPE")) type = std::atoi(ft);
	const int used = text::encode(type, q, d.data(), cap, !p3_);
	if (size_t(used) >= q.size()) { text::RleCompressor r = rle_tx_; q += r.flush(); }
	const bool more = q.size() > size_t(used);
	st = uint8_t((pending_status_ & ~0x3c) | type << 2 | more_bit(more, q.size(), cap));
	d.resize(size_t(tx_payload()), 0x1e);
	return true;
}

void P2Station::premod_after_bk()
{
	static const bool old = std::getenv("P3_BK_PREMOD_OLD") != nullptr;
	if (old || !p3_ || !premod_on() || !cfg_.realtime || pool_.size() == 0) return;
	premod_.clear();
	const int lv0 = p3_level_, ctr0 = counter_; const bool lng0 = p3_long_, first0 = p3_first_tx_, qe0 = qrt_end_sent_;
	const float fs = cfg_.fs, lo = cfg_.tone_lo, hi = cfg_.tone_hi;
	for (int lv : {5, 3, 1}) {                       // as case 20 on T, S, P
		p3_level_ = lv; p3_long_ = false; counter_ = 2; p3_first_tx_ = true; qrt_end_sent_ = false;
		std::vector<uint8_t> d; uint8_t st = 0;
		PremodKey k;
		if (!predict_new(d, st) || !premod_key(tx_words(d, st, (counter_ + 1) & 3, true), k)) continue;
		premod_.push_back(Premod{k, pool_.submit([k, fs, lo, hi]() { return premod_make(k, fs, lo, hi); }).share()});
	}
	p3_level_ = lv0; counter_ = ctr0; p3_long_ = lng0; p3_first_tx_ = first0; qrt_end_sent_ = qe0;
}

void P2Station::send_bk(int64_t start)
{
	const int n = p2::data_bytes(level_, 7);
	pending_used_ = int(std::min<size_t>(size_t(n), txq_.size()));
	std::vector<uint8_t> data(size_t(n), 0x1e), info(size_t(n + 3));
	for (int i = 0; i < pending_used_; i++) data[size_t(i)] = uint8_t(txq_[size_t(i)]);
	p2::make_info(level_, data.data(), 0x00, info.data(), 7);
	std::vector<uint8_t> w{0x6a};
	for (const char *p = p2arq::kBkPattern; *p; p++) w.push_back(*p == 'a' ? 0xaa : 0x22);
	const auto dw = p2::encode_data(level_, info.data(), 7);
	w.insert(w.end(), dw.begin(), dw.end());
	w.push_back(p2arq::kBkTail[level_]);
	queue_psk(start, w);
	my_start_ = start;
	if (pending_used_ > 0) sent_text_ = true;
	log("break-in");
}

void P2Station::queue_p3(int64_t slot, const std::vector<uint8_t> &words_in)
{
	const std::vector<uint8_t> &words = p3_test_skew(words_in);   // test hook (fix 263)
	int64_t start; double lead;
	p3_place(p3_pulse0(slot, 5, false), 0.02, start, lead);
	TxBurst tb;
	// Fix 80: made on a worker in real time (13 ms on the audio loop every
	// cycle we send: with a shorter playback prime it underran).
	if (cfg_.realtime && !std::getenv("P3_REPLAY")) {
		const float fs = cfg_.fs, amp = kP3DataGain * p3_data_amp(), clip = p3_clip_now();
		const bool sw = psk_swap_;
		tb.fut = premod_take(PremodKey{TX_P3, 5, false, sw, amp, 0.0, words, lead, clip}, &tb.tr);
		if (!tb.fut.valid()) {
			tb.tr = std::make_shared<TxTrace>();
			tb.fut = pool_.submit(traced(tb.tr, [words, fs, sw, amp, lead, clip]() { return mod_p3(words, fs, sw, amp, lead, clip); }), true).share();
		} else if (tb.tr) tb.tr->hit = true;
		if (tb.tr) tb.tr->queued_n = n_;
	} else {
		if (premod_check()) premod_take(PremodKey{TX_P3, 5, false, psk_swap_, kP3DataGain * p3_data_amp(), 0.0, words, lead, p3_clip_now()});
		tb.audio = mod_p3(words, cfg_.fs, psk_swap_, kP3DataGain * p3_data_amp(), lead, p3_clip_now());
	}
	tb.start = start;
	// Test hook: P3_REPLAY=FILE.raw:SAMPLE replaces this packet's audio by a
	// recording (s16, 9600 Hz) whose slot starts at SAMPLE, aligned to ours.
	if (const char *rp = std::getenv("P3_REPLAY")) {
		std::string spec(rp);
		const size_t c = spec.rfind(':');
		if (FILE *f = std::fopen(spec.substr(0, c).c_str(), "rb")) {
			const double s0 = std::atof(spec.c_str() + c + 1);
			const long pre = 480, len = 9600;
			std::fseek(f, long(std::floor(s0) - pre) * 2, SEEK_SET);
			std::vector<int16_t> b(static_cast<size_t>(len));
			const size_t got = std::fread(b.data(), 2, b.size(), f);
			std::fclose(f);
			tb.audio.assign(b.begin(), b.begin() + long(got));
			tb.start = slot - pre;
			log("P3 packet replaced by the recording");
		}
	}
	psk_swap_ = !psk_swap_;
	tx_push(std::move(tb));
}

int P2Station::p3_test_skew_step() const
{
	static double at = -1, until = -1, step0 = 1;
	static const bool on = [] { const char *e = std::getenv("P3_TEST_TX_SKEW"); return e && std::sscanf(e, "%lf:%lf:%lf", &at, &until, &step0) >= 2; }();
	const double t = double(n_) / cfg_.fs;
	if (!on || t < at || t >= until) return 0;
	return std::max(1, int(step0) + int((t - at) / 10.0));
}

const std::vector<uint8_t> & P2Station::p3_test_skew(const std::vector<uint8_t> &words)
{
	const int c = p3_test_skew_step();
	if (!c) return words;
	uint64_t z = 0xCBF29CE484222325ull ^ uint64_t(c);
	for (uint8_t v : words) { z ^= v; z *= 0x100000001B3ull; }
	std::vector<uint8_t> &w = skew_w_;
	w = words;
	for (auto &v : w) {
		z ^= z << 13; z ^= z >> 7; z ^= z << 17;
		const int r = int(z % 100), st = r < 90 ? c - 1 + int((z >> 8) % 3) : int((z >> 16) & 15);
		v = uint8_t((v & 0xF0) | (st & 15));
	}
	return w;
}

void P2Station::queue_lv(int64_t slot, int level, const std::vector<uint8_t> &words_in, bool lng)
{
	const std::vector<uint8_t> &words = p3_test_skew(words_in);   // test hook (fix 263)
	int64_t start; double lead;
	p3_place(p3_pulse0(slot, level, lng), 0.03, start, lead);
	TxBurst tb;
	if (cfg_.realtime && (lng || !std::getenv("P3_LV_SYNC_MOD"))) {   // fix 80: short packets too
		// A long packet (~3.4 s of audio) made on the audio loop with a
		// control-signal look in the same callback underran playback on the
		// RF bench (KB5LZK, 2026-10-16 16:27: our first SL2 long packet
		// spoilt, the speed-up then misread). Made on a worker, as P2's.
		const float fs = cfg_.fs, amp = lv_gain(level) * p3_data_amp();
		const bool sw = psk_swap_;
		tb.fut = premod_take(PremodKey{TX_LV, level, lng, sw, amp, 0.0, words, lead}, &tb.tr);
		if (!tb.fut.valid()) {
			tb.tr = std::make_shared<TxTrace>();
			tb.fut = pool_.submit(traced(tb.tr, [level, words, fs, sw, amp, lead, lng]() { return mod_lv(level, words, fs, sw, amp, lead, lng); }), true).share();
		} else if (tb.tr) tb.tr->hit = true;
		if (tb.tr) tb.tr->queued_n = n_;
	} else {
		if (premod_check()) premod_take(PremodKey{TX_LV, level, lng, psk_swap_, lv_gain(level) * p3_data_amp(), 0.0, words, lead});
		tb.audio = mod_lv(level, words, cfg_.fs, psk_swap_, lv_gain(level) * p3_data_amp(), lead, lng);
	}
	// The per-level pulse offset applies to short packets only: with it,
	// our long packets arrived ~23 samples late at SL3 against the
	// firmware's own long packets.
	tb.start = start;
	psk_swap_ = !psk_swap_;
	tx_push(std::move(tb));
}

void P2Station::late_watch_poll()
{
	if (!late_watch_ || !late_watch_done_) return;
	const int64_t done = late_watch_done_->load();
	if (!done) return;
	const DecodeTimes &t = *late_watch_;
	auto ms = [&](int64_t v) { if (!v) return std::string("-"); char b[32]; std::snprintf(b, sizeof b, "%.1f", double(v - t.launch) / 1e6); return std::string(b); };
	std::string lv;
	for (int i = 0; i < t.n; i++) {
		const int64_t a = t.s[i].load(), b = t.e[i].load();
		lv += (i ? "; SL" : "SL") + std::to_string(t.lv[i]) + " ran +" + ms(a) + "..+" + ms(b) + " ms";
	}
	log("P3: the late decode (expecting SL" + std::to_string(late_watch_lv_) + "), from its launch: " + lv + "; result at +" + ms(done) + " ms, the deadline was +" + ms(late_watch_dl_) +
	    " ms (" + std::to_string(std::lround(double(done - late_watch_dl_) / 1e6)) + " ms past it); a level's own work is its ran span, a late start is waiting (other work or scheduling)");
	late_watch_.reset(); late_watch_done_.reset();
}

std::vector<int> P2Station::p3_levels_now(int64_t slot, bool *trimmed) const
{
	auto v = p3_try_levels(p3_rx_level_, rx_t_state_);
	static const bool lastlv_off = std::getenv("P3_LASTLV_OFF") != nullptr;   // test: off
	if (!p3_drift_off() && !lastlv_off && misses_ >= 2 && p3_last_ok_level_ >= 2 && p3_last_ok_level_ <= 6 && std::find(v.begin(), v.end(), p3_last_ok_level_) == v.end())
		v.push_back(p3_last_ok_level_);
	if (v.size() > 1 && p3_fb_steady() && cpu_fb_at(slot)) { v.resize(1); if (trimmed) *trimmed = true; }
	return v;
}

std::vector<int> P2Station::p3_try_levels(int lv, bool up)
{
	std::vector<int> v;
	if (lv == 5 || lv < 2) v.push_back(5);
	if (lv < 2) return v;
	const int top = lv == 6 ? 6 : std::min(lv + (up && lv != 4 ? 1 : 0), 4);
	for (int l = top; l >= std::max(2, top - (up ? 2 : 1)); l--)
		if (l != 5) v.push_back(l);
	if (lv == 6) v.push_back(5);
	return v;
}

std::vector<float> P2Station::p3_window(int64_t a, int64_t b) const
{
	std::vector<float> x(size_t(std::max<int64_t>(b - a, 1)), 0.0f);
	const bool in_ring = a >= 0 && n_ - a < kRing - 2;
	if (in_ring)
		for (int64_t t = a; t < b && t < n_; t++) x[size_t(t - a)] = raw_[size_t(t % kRing)];
	if (std::fabs(p3_rx_df_) > 3.0) {                // the peer's measured offset (fix 49): its own sign convention
		if (in_ring) shift_cached(x, p3_rx_df_, cfg_.fs, a, long(std::min(b, n_) - a), p3_shift_cache_);
		else { shift_audio(x, p3_rx_df_, cfg_.fs); p3_shift_cache_ = ShiftCache{}; }
	}
	return x;
}

void P2Station::p3_df_seed()
{
	fs_poll();                                     // a frequency search finished since (2026-11-14)
	static const bool off = std::getenv("P3_NO_DF_SEED") != nullptr;
	if (p3_df_set_ || off) return;
	p3_df_set_ = true;
	if (std::fabs(afc_hz()) <= 3.0) return;
	p3_rx_df_ = -afc_hz();
	log("P3: receive shift " + std::to_string(p3_rx_df_) + " Hz from the AFC (no break-in measurement)");
}

void P2Station::fs_decoded(int level)
{
	fs_fails_ = 0; fs_wait_ = kFsMinFails; fs_prev_ = {}; fs_prev2_ = {};
	if (fs_trial_) {
		if (level < 2) return;
		fs_trial_ = false;
		log("P3: receive shift " + std::to_string(-p3_rx_df_) + " Hz confirmed: a packet decoded at SL" + std::to_string(level) + " (tried " + std::to_string(fs_trial_hz_) + " Hz from the spectrum)");
	}
	fs_anchor_set();
}

void P2Station::fs_failed()
{
	if (fs_fails_ < (1 << 20)) fs_fails_++;
	if (!fs_trial_ || ++fs_trial_fails_ < kFsTrial) return;
	fs_trial_ = false;
	const bool untouched = std::fabs(-p3_rx_df_ - fs_trial_hz_) < 1e-9;   // nothing else moved it meanwhile
	// Fix 265 (TODO ff 2): back to the last DECODED shift (fix 261's anchor), not to the shift in use before
	// the try (on the air that was the set-up's, ~10 Hz off: n4a10 "back to 8.75 Hz" with 1.66 decoded).
	// P3_RXSHIFT_BACK_OLD=1: back to the shift before the try.
	static const bool back_old = std::getenv("P3_RXSHIFT_BACK_OLD") != nullptr;
	if (untouched) { p3_rx_df_ = (fs_anchor_ok_ && !back_old) ? -fs_anchor_ : fs_trial_was_df_; afc_ = fs_trial_was_afc_; }
	fs_wait_ = std::min(kFsMaxWait, fs_wait_ * 2); fs_fails_ = 0; fs_prev_ = {}; fs_prev2_ = {};
	log("P3: receive shift " + std::to_string(fs_trial_hz_) + " Hz not confirmed: " + std::to_string(kFsTrial) + " packets failed at it; " +
	    (untouched ? "back to " + std::to_string(-p3_rx_df_) + " Hz" : "the shift was set meanwhile, left at " + std::to_string(-p3_rx_df_) + " Hz") +
	    "; the next try after " + std::to_string(fs_wait_) + " failed packets");
}

auto P2Station::fs_measure(const std::vector<float> &x, double fs, double centre, int kind, int64_t slot, bool p3, double lo, double hi, bool old) -> FsResult
{
	static const fmodem::fsearch::PulsePower pw_flat(p3::kPulse9600, p3::kPulseHalf9600), pw_cs(p3::kCsPulse9600, p3::kCsPulseHalf9600);
	FsResult best; best.slot = slot; best.kind = kind;
	const auto t0 = std::chrono::steady_clock::now();
	for (int t = 0; t < 4; t++) {
		if (p3 ? (t == 3 || (kind == 1 && t != 0)) : t != 3) continue;
		double car[18], wt[18]; int nc = 0;
		const fmodem::fsearch::PulsePower *pw = &pw_cs;
		double range = 70;
		if (t == 0) { car[0] = p3::kCsLo; car[1] = p3::kCsHi; nc = 2; range = 100; }
		else if (t == 1) { const auto &L = p3::layout(2); for (int j = 0; j < L.sec[0].n; j++) car[nc++] = p3::carrier_hz(L.sec[0].even[j]); }
		else if (t == 2) { for (int k = 0; k < 18; k++) car[nc++] = p3::carrier_hz(k); pw = &pw_flat; }
		else { car[0] = lo; car[1] = hi; nc = 2; range = 90; pw = &fs_p2_pulse(); }
		for (int j = 0; j < nc; j++) wt[j] = 1.0;
		// two-carrier bursts: around 0 (their AFC may already sit on a wrong alias); the P3 comb
		// repeats every 120 Hz: around the shift in use
		const double c = nc == 2 ? 0.0 : centre;
		const auto r = fmodem::fsearch::burst_offset(x.data(), long(x.size()), fs, car, wt, nc, *pw, c, range);
		const bool clear = old || nc == 2 || r.comb >= kFsComb;   // fix 261
		if (r.ok && clear && r.match > best.match) { best.ok = true; best.hz = r.hz; best.match = r.match; best.comb = r.comb; best.tmpl = t; best.centre = c; best.edge = std::fabs(r.hz - c) > range - kFsEdge; }
	}
	best.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	return best;
}

void P2Station::fs_submit(int64_t slot, int64_t a, int64_t b, int kind)
{
	if (fstep_old()) return;
	fs_poll();
	if (p3_ && kind == 0 && fs_trial_on()) fs_failed();   // fix 263
	if (fs_fut_.valid()) return;                    // one at a time (a few ms each)
	const bool old = rxshift_old();
	if (p3_ && kind == 0 && !fs_anchor_ok_) fs_anchor_set();   // no decode yet: the set-up's shift (no re-measure can precede this)
	const double centre = (!old && p3_ && kind == 0 && fs_anchor_ok_) ? fs_anchor_ : fs_in_use(kind);   // fix 261
	const double fs = cfg_.fs, lo = cfg_.tone_lo, hi = cfg_.tone_hi;
	const bool p3 = p3_;
	if (!cfg_.realtime) { std::promise<FsResult> pr; pr.set_value(fs_measure(fs_window(a, b), fs, centre, kind, slot, p3, lo, hi, old)); fs_fut_ = pr.get_future().share(); fs_poll(); return; }
	auto x = std::make_shared<const std::vector<float>>(fs_window(a, b));
	fs_fut_ = pool_.submit([x, fs, centre, kind, slot, p3, lo, hi, old]() { return fs_measure(*x, fs, centre, kind, slot, p3, lo, hi, old); }).share();
}

void P2Station::fs_poll()
{
	if (!fs_fut_.valid() || fs_fut_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
	const FsResult r = fs_fut_.get();
	fs_fut_ = {};
	if (state_ == ST_IDLE || state_ == ST_LISTEN) { fs_prev_ = {}; return; }
	const double cur = fs_in_use(r.kind);
	const double near = r.kind == 0 ? 8.0 : 15.0;
	if (debug_) log(std::string(p3_ ? "P3" : "P2") + ": frequency search (" + std::string(r.kind ? "control signal" : "data packet") + ", template " + std::to_string(r.tmpl) + "): " +
	                (r.ok ? std::to_string(r.hz) + " Hz, match " + std::to_string(r.match) + ", comb " + std::to_string(r.comb) : std::string("nothing")) + " against " + std::to_string(cur) + " Hz in use, " + std::to_string(r.ms) + " ms");
	if (!r.ok || r.match < kFsMatch || std::fabs(r.hz - cur) <= near) { fs_prev_ = {}; fs_prev2_ = {}; return; }
	const bool trial = p3_ && r.kind == 0 && fs_trial_on();   // fix 263
	const std::string scores = "match " + std::to_string(r.match) + ", comb " + std::to_string(r.comb) + ", template " + std::to_string(r.tmpl);
	if (trial && r.edge) {
		log("P3: receive shift reading " + std::to_string(r.hz) + " Hz not taken: within " + std::to_string(int(kFsEdge)) + " Hz of the search window's edge (window around " + std::to_string(r.centre) + " Hz; " + scores + ")");
		fs_prev_ = {}; fs_prev2_ = {};
		return;
	}
	const int64_t gap = r.slot - fs_prev_.slot;
	if (fs_prev_.ok && fs_prev_.kind == r.kind && gap > 0 && gap <= samples(2.5 * lcyc_s()) && std::fabs(fs_prev_.hz - r.hz) <= 5.0) {
		const double hz = 0.5 * (fs_prev_.hz + r.hz);
		const double was = cur;
		if (trial) {
			const double from = fs_anchor_ok_ ? fs_anchor_ : cur;
			const bool three = fs_prev2_.ok && fs_prev2_.kind == r.kind && r.slot - fs_prev2_.slot <= samples(5.0 * lcyc_s()) &&
			                   std::max({fs_prev2_.hz, fs_prev_.hz, r.hz}) - std::min({fs_prev2_.hz, fs_prev_.hz, r.hz}) <= 5.0;
			// Fix 265 (TODO ff 2): a candidate within kFsNearDec of the last decoded shift is not thinned out
			// (n4a10: -0.12, -0.04, -0.07 Hz against 1.66 decoded "not tried: only 1/2/3 failed packets (8
			// needed)"); kFsMinFails as for a first try. P3_RXSHIFT_THIN_OLD=1: thinned as any other.
			static const bool thin_old = std::getenv("P3_RXSHIFT_THIN_OLD") != nullptr;
			const bool near_dec = !thin_old && fs_anchor_ok_ && std::fabs(hz - fs_anchor_) <= kFsNearDec;
			std::string why;
			if (fs_trial_) why = "a try is already running (" + std::to_string(fs_trial_hz_) + " Hz)";
			else if (fs_fails_ < (near_dec ? kFsMinFails : fs_wait_)) why = "only " + std::to_string(fs_fails_) + " failed packets since the last decode (" + std::to_string(near_dec ? kFsMinFails : fs_wait_) + " needed)";
			else if (std::fabs(hz - from) > kFsCap && !three) why = std::to_string(std::lround(std::fabs(hz - from))) + " Hz from the decoded shift, a third agreeing reading needed";
			if (!why.empty()) {
				log("P3: receive shift candidate " + std::to_string(hz) + " Hz not tried: " + why + " (decoded shift " + std::to_string(from) + " Hz; " + scores + ")");
				fs_prev2_ = fs_prev_; fs_prev_ = r; fs_prev_.ok = true;
				return;
			}
			fs_trial_ = true; fs_trial_fails_ = 0; fs_trial_hz_ = hz; fs_trial_was_df_ = p3_rx_df_; fs_trial_was_afc_ = afc_;
			p3_rx_df_ = -hz; p3_df_set_ = true;
			afc_ = hz / kAfcHzPerUnit;
			fs_reseeds_++;
			log("P3: receive shift re-measured from the bursts' spectrum: " + std::to_string(hz) + " Hz, tried until a packet decodes (was " + std::to_string(was) + " Hz, decoded shift " + std::to_string(from) + " Hz; " + scores + ")");
			fs_prev_ = {}; fs_prev2_ = {};
			return;
		}
		p3_rx_df_ = -hz; p3_df_set_ = true;
		afc_ = hz / kAfcHzPerUnit;
		fs_reseeds_++;
		log(std::string(p3_ ? "P3: " : "P2: ") + (r.kind == 0 ? "receive shift re-measured" : "control-signal shift re-measured") + " from the bursts' spectrum: " +
		    std::to_string(hz) + " Hz (was " + std::to_string(was) + " Hz; match " + std::to_string(r.match) + ", comb " + std::to_string(r.comb) + ", template " + std::to_string(r.tmpl) + ")");
		fs_prev_ = {};
		return;
	}
	fs_prev2_ = fs_prev_; fs_prev_ = r; fs_prev_.ok = true;
}

void P2Station::launch_p3_sync(int64_t slot, double search_ms)
{
	p3_df_seed();
	const int64_t a = slot - samples(0.06), b = slot + samples(kP3Sync);
	const auto x = std::make_shared<const std::vector<float>>(p3_window(a, b));
	const double base = double(slot) + 0.8 * T_ - double(a);
	const auto lvs = p3_levels_now(slot);
	const double fs = cfg_.fs;
	p3_early_ = P3EarlySync{};
	p3_early_.slot = slot; p3_early_.lvs = lvs; p3_early_.df = p3_rx_df_;
	// Fix 152: with the SL5 timing found, the data-independent part of its demodulation
	// (the factor) is made here too, while the packet is still arriving: for launch_p3's
	// window (same start a, ending at slot + kP3Launch).
	const long n_dec = long(std::max<int64_t>(slot + samples(kP3Launch) - a, 1));
	static const bool no_plan = std::getenv("P3_NO_PLAN") != nullptr;
	static const bool prof = std::getenv("P3_PROF") != nullptr;
	const int ng = p3_sync_groups(lvs.size());
	const auto w0 = std::chrono::steady_clock::now();
	const bool order = !p3_sched_old();
	for (int g = 0; g < ng; g++) {
		std::vector<int> gl, gp;
		for (size_t i = size_t(g); i < lvs.size(); i += size_t(ng)) { gl.push_back(lvs[i]); gp.push_back(p3_prio(2 + int(i))); }
		auto pr = std::make_shared<std::vector<std::promise<P3LvSync>>>(gl.size());
		for (size_t k = 0; k < gl.size(); k++) p3_early_.f.emplace_back(gl[k], (*pr)[k].get_future().share());
		(order ? sync_pool_ : pool_).submit([x, gl, gp, order, pr, fs, base, search_ms, n_dec, w0, g, ng]() {
			const auto s0 = std::chrono::steady_clock::now();
			for (size_t k = 0; k < gl.size(); k++) {
				const int l = gl[k];
				std::unique_ptr<par::FifoPrioScope> ps;
				if (order) ps.reset(new par::FifoPrioScope(gp[k]));   // below the decodes (above)
				P3LvSync r = p3_sync_lv(l, x->data(), long(x->size()), fs, base, search_ms);
				if (!no_plan) {
					if (l == 5) r.plan5 = std::make_shared<const p3::DemodPlan>(p3::demod_plan(n_dec, fs, r.s5.tb, 0, p3::kAllPulses, r.s5.hb.bias / (16 * p3::kT)));
					else if (!p3::layout(l).narrow) r.plan = std::make_shared<const p3::FlatPlan>(p3::demod_flat_plan(l, n_dec, fs, r.s.t, 0));
				}
				(*pr)[k].set_value(std::move(r));
			}
			if (prof) {
				std::string ls;
				for (int l : gl) ls += (ls.empty() ? "" : ",") + std::to_string(l);
				std::fprintf(stderr, "P3 early sync: group %d of %d, levels %s: started %.1f ms after launch, took %.1f ms\n", g + 1, ng, ls.c_str(),
				             std::chrono::duration<double, std::milli>(s0 - w0).count(), std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s0).count());
			}
		});
	}
}

auto P2Station::submit_sl1(int64_t slot, bool in_case) -> std::future<RxPacket>
{
	const bool dbg = debug_;
	const int prio = in_case && !p3_sched_old() ? p3_prio(1) : 0;
	return pool_.submit([this, slot, dbg, prio, sp = snapshot_short(slot), tv = trk_view(slot)]() {
		std::unique_ptr<par::FifoPrioScope> ps;
		if (prio > 0) ps.reset(new par::FifoPrioScope(prio));
		const auto w0 = std::chrono::steady_clock::now();
		RxPacket r = find_packet(slot, false, false, &sp, nullptr, &tv);
		static const bool prof = std::getenv("P3_PROF") != nullptr;
		if (dbg || prof) std::fprintf(stderr, "P3 SL1 worker: decode %.1f ms, ok %d\n", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - w0).count(), int(r.ok));
		return r;
	});
}

void P2Station::launch_sl1(int64_t slot)
{
	// Fix 216b: also while packets fail now and then (one of the last kSl1Recent failed). The try after
	// a failure is otherwise started at the look, ~3-10 ms before our answer, and was never in time on
	// the Pi 4 (f216-13, SL6 with every first copy failing and its repeat decoded by memory ARQ: 125
	// "SL1 decode not ready" of 125). P3_SL1_EARLY_OLD=1: as before.
	static const bool early_old = std::getenv("P3_SL1_EARLY_OLD") != nullptr;
	const bool recent = !early_old && p3_since_fail_ < kSl1Recent;
	if (cfg_.realtime && p3_ && (p3_rx_level_ <= 2 || misses_ > 0 || recent) && sl1_slot_ != slot) {
		sl1_slot_ = slot;
		sl1_fut_ = submit_sl1(slot, p3_rx_level_ > 2);
	}
}

void P2Station::launch_p3(int64_t slot, double search_ms)
{
	p3_df_seed();
	reap_stale_decodes();
	if (p3_fut_.valid()) {
		if (!cfg_.realtime || p3_fut_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) p3_fut_.get();
		else stale_futs_.push_back(std::move(p3_fut_));
	}
	const int64_t a = slot - samples(0.06), b = slot + samples(kP3Launch);
	const auto x = std::make_shared<const std::vector<float>>(p3_window(a, b));
	const double fs = cfg_.fs, base = double(slot) + 0.8 * T_ - double(a);   // word-0 centre in x
	bool trimmed = false;
	const auto lvs = p3_levels_now(slot, &trimmed);
	if (trimmed) fb_note(0, load_.fb_p3_levels);
	// the early timing search, if it was for this slot. Fix 236: per level; a level searched early is
	// used when the level set is the one searched (as before), or else when the receive shift has not
	// changed since (the same search on the same audio: before, every level searched again then)
	P3EarlySync es;
	if (p3_early_.slot == slot) es = std::move(p3_early_);
	p3_early_ = P3EarlySync{};
	const bool es_use = es.slot == slot && (es.lvs == lvs || es.df == p3_rx_df_);
	const bool dbg = debug_;
	const int mq_lv = p3_rx_level_, mq_cnt = (last_rx_counter_ + 1) & 3;
	const auto mq_store = std::make_shared<const std::vector<MqCopy>>(mq3_);
	p3_fut_slot_ = slot;
	// The SL1 try (P2 L1 + scrambler, 8-40 ms) starts now too, beside the
	// level decodes, when it may be needed: receiving at SL1/SL2, or the
	// last packet failed. Started only after the main decode had failed,
	// it had ~17 ms before our answer and never finished: on the air
	// (N5TW, 2026-10-15) every SL1 packet after the RMS stepped down was
	// thrown away as "not ready" (all 14 decode offline) and the link
	// timed out. An SL1 packet is in by slot + 0.83 s (same decodes).
	launch_sl1(slot);
	const auto w0 = p3_launch_wall_ = std::chrono::steady_clock::now();
	const auto jt = p3_times_ = std::make_shared<DecodeTimes>();
	jt->launch = wall_ns(); jt->n = int(std::min(lvs.size(), DecodeTimes::kMax));
	// one task per candidate level (in parallel), then one taking the
	// first that decodes; if none, the (failed) SL5 try's timing
	std::vector<std::shared_future<p3::RxPacket>> fin;
	// a level's decode, run by whichever comes first: its worker or the task choosing (above)
	struct LvJob { std::atomic<bool> claimed{false}; std::packaged_task<p3::RxPacket()> task; };
	std::vector<std::shared_ptr<LvJob>> jobs;
	const bool sched_old = p3_sched_old();
	bool early = false;
	for (size_t i = 0; i < lvs.size(); i++) {
		const int l = lvs[i];
		std::shared_future<P3LvSync> sf;
		if (es_use) for (const auto &e : es.f) if (e.first == l) sf = e.second;
		early = early || sf.valid();
		auto job = [x, fs, a, base, search_ms, l, sf, mq_lv, mq_cnt, mq_store, w0]() {
			const float *xd = x->data();
			const long n = long(x->size());
			static const bool prof = std::getenv("P3_PROF") != nullptr;   // where a decode's time goes (slow CPUs: the Pi 4)
			const auto q0 = std::chrono::steady_clock::now();
			P3LvSync sy;
			if (sf.valid()) sy = sf.get();
			const auto q1 = std::chrono::steady_clock::now();
			if (!sf.valid()) sy = p3_sync_lv(l, xd, n, fs, base, search_ms);   // no early search for this level
			const auto q2 = std::chrono::steady_clock::now();
			struct ProfOut { std::chrono::steady_clock::time_point w0, q0, q1, q2; int l; ~ProfOut() { if (prof) std::fprintf(stderr, "P3 level %d: started %.1f ms after launch, sync wait %.1f ms, own sync %.1f ms, finish %.1f ms\n", l,
				std::chrono::duration<double, std::milli>(q0 - w0).count(),
				std::chrono::duration<double, std::milli>(q1 - q0).count(), std::chrono::duration<double, std::milli>(q2 - q1).count(), std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - q2).count()); } } po{w0, q0, q1, q2, l};
			p3::RxPacket r;
			// memory ARQ: copies of this packet stored at this level (oldest first)
			std::vector<const MqCopy *> st;
			if (l == mq_lv && !std::getenv("P3_NO_MARQ")) for (const auto &c : *mq_store) if (c.level == l) st.push_back(&c);
			if (l == 5) {
				const bool want = l == mq_lv && !std::getenv("P3_NO_MARQ");
				std::vector<std::complex<double>> dd;
				r = p3::finish_at(xd, n, fs, sy.s5, want ? &dd : nullptr, sy.plan5.get());
				r.t0 += double(a);
				r.hscore = 5;
				if (!r.ok && want) {
					r.mq_level = 5; r.mq_d0 = std::move(dd);
					if (!st.empty()) {
						auto sum = r.mq_d0;
						for (const auto *c : st) for (size_t w = 0; w < sum.size() && w < c->d0.size(); w++) sum[w] += c->d0[w];
						const p3::RxResult rr = p3::sl5_decode_diffs(sum);
						if (rr.ok && (rr.info[p3::kDataBytes] & 3) == mq_cnt) { r.r = rr; r.ok = true; r.marq = int(st.size()) + 1; }
					}
				}
				return r;
			}
			const bool want = l == mq_lv && !std::getenv("P3_NO_MARQ");
			auto q = p3::finish_level(l, xd, n, fs, sy.s, 0, want ? &r.mq_d0 : nullptr, want ? &r.mq_d1 : nullptr, sy.plan.get());
			if (!q.ok && want) {
				r.mq_level = l;
				// the carrier map alternates with every transmission: two
				// combinations (which map the newest copy has)
				for (int m0 = 0; m0 < 2 && !st.empty() && !q.ok; m0++) {
					auto sum = m0 ? r.mq_d1 : r.mq_d0;
					for (size_t c = 0; c < st.size(); c++) {
						const bool odd = ((st.size() - c) & 1) ? !m0 : m0;   // one map flip per transmission back
						const auto &d = odd ? st[c]->d1 : st[c]->d0;
						for (size_t w = 0; w < sum.size() && w < d.size(); w++) sum[w] += d[w];
					}
					const p3::Lv23Rx rr = p3::decode_level_diffs(l, sum);
					if (rr.ok && (rr.status & 3) == mq_cnt) {
						q.ok = true; q.data = rr.data; q.status = rr.status; q.t0 = sy.s.t; q.step_bias = 0; q.agree = rr.agree;
						r.marq = int(st.size()) + 1;
					}
				}
			}
			if (!q.ok) return r;
			r.ok = true;
			r.agree = q.agree;
			r.resid = q.resid;                         // the fit's residual (noise / signal): long-request gate
			r.lvdata = q.data;
			r.lvstatus = q.status;
			r.df = kLvBiasHz_() * q.step_bias;         // residual offset (fix 70)
			std::memset(r.r.info, 0x1e, sizeof r.r.info);
			for (size_t j = 0; j < q.data.size() && j < size_t(p3::kDataBytes); j++) r.r.info[j] = q.data[j];
			r.r.info[p3::kDataBytes] = q.status;
			r.t0 = q.t0 + double(a) - p3::level_offset_s(l) * fs + kP3Off * fs;   // so p3_slot() gives the slot
			r.hscore = l;
			return r;
		};
		if (i < DecodeTimes::kMax) jt->lv[i] = l;
		auto timed = [job = std::move(job), jt, i]() {   // (log-only: when each level's decode ran)
			if (i < DecodeTimes::kMax) jt->s[i] = wall_ns();
			auto r = job();
			if (i < DecodeTimes::kMax) jt->e[i] = wall_ns();
			return r;
		};
		if (sched_old) { fin.push_back(pool_.submit(timed).share()); continue; }
		auto j = std::make_shared<LvJob>();
		j->task = std::packaged_task<p3::RxPacket()>(std::move(timed));
		fin.push_back(j->task.get_future().share());
		jobs.push_back(std::move(j));
	}
	auto done = p3_done_ = std::make_shared<std::atomic<int64_t>>(0);
	p3_fut_ = pool_.submit([fin, jobs, lvs, dbg, w0, early, mq_lv, done]() {
		p3::RxPacket out;
		const p3::RxPacket *mq = nullptr;
		for (size_t i = 0; i < fin.size(); i++) {
			if (i < jobs.size() && !jobs[i]->claimed.exchange(true)) jobs[i]->task();   // not started by a worker: here
			const auto &r = fin[i].get();
			if (r.ok) { out = r; mq = nullptr; break; }
			if (lvs[i] == 5) out = r;
			if (lvs[i] == mq_lv && r.mq_level) mq = &r;
		}
		if (mq && !out.ok) { out.mq_level = mq->mq_level; out.mq_d0 = mq->mq_d0; out.mq_d1 = mq->mq_d1; }
		static const bool prof = std::getenv("P3_PROF") != nullptr;
		if (dbg || prof) std::fprintf(stderr, "P3 worker: levels %zu early-sync %d took %.1f ms\n", lvs.size(), int(early), std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - w0).count());
		// test only: a slow computer (the back-off test): P3_TEST_DECODE_DELAY_MS more per
		// decode when a candidate level is P3_TEST_SLOW_FROM or higher (default 1: all)
		static const int t_dly = std::getenv("P3_TEST_DECODE_DELAY_MS") ? std::atoi(std::getenv("P3_TEST_DECODE_DELAY_MS")) : 0;
		static const int t_from = std::getenv("P3_TEST_SLOW_FROM") ? std::atoi(std::getenv("P3_TEST_SLOW_FROM")) : 1;
		if (t_dly > 0 && !lvs.empty() && *std::max_element(lvs.begin(), lvs.end()) >= t_from) std::this_thread::sleep_for(std::chrono::milliseconds(t_dly));
		// fix 226, test only: ONE slow decode (the bench's f223-7 hiccup, an SL5 decode of 225 ms):
		// the P3_TEST_SLOW_ONCE-th P3 decode of the process (counted from 1) takes
		// P3_TEST_SLOW_ONCE_MS (default 250) more
		static const long t_once = std::getenv("P3_TEST_SLOW_ONCE") ? std::atol(std::getenv("P3_TEST_SLOW_ONCE")) : 0;
		static const int t_once_ms = std::getenv("P3_TEST_SLOW_ONCE_MS") ? std::atoi(std::getenv("P3_TEST_SLOW_ONCE_MS")) : 250;
		static std::atomic<long> t_count{0};
		if (t_once > 0 && ++t_count == t_once) { std::fprintf(stderr, "P3 test: this decode made %d ms slower (P3_TEST_SLOW_ONCE)\n", t_once_ms); std::this_thread::sleep_for(std::chrono::milliseconds(t_once_ms)); }
		done->store(wall_ns());
		return out;
	});
	for (const auto &j : jobs) pool_.submit([j]() { if (!j->claimed.exchange(true)) j->task(); });
}

bool P2Station::match_words(int64_t exp, const uint8_t *w, int n, double &start, int64_t heard_upto)
{
	Demod d0 = demod_cs(exp - samples(0.04), exp + samples(0.30), n + 2, nullptr, nullptr, afc_);
	if (!d0.ok) return false;
	const int64_t nn = heard_upto >= 0 ? std::min(heard_upto, n_) : n_;
	const int64_t to_x = setup_heard_off() ? exp + samples(0.30) : std::max(exp + samples(0.30), nn - int64_t(2 * T_));
	if (to_x > exp + samples(0.30)) {               // heard late: the found burst's pulses from all that is heard
		const double cc[2] = {d0.c[0], d0.c[1]};
		const Demod dx = demod_cs(exp - samples(0.04), to_x, n + 2, nullptr, cc, afc_);
		if (dx.ok) d0 = dx;
	}
	// Test hook (fix 235): P3_TEST_SETUP_HI_EARLY=K: the high carrier's first pulse found K pulses
	// early (K may be fractional), as on the bench (f232-2: timing lo +70 hi -359 samples).
	if (const char *e = std::getenv("P3_TEST_SETUP_HI_EARLY"); e && state_ == ST_P3SETUP) {
		double cc[2] = {d0.c[0], d0.c[1] - std::atof(e) * T_};
		d0 = demod_cs(exp - samples(0.04), to_x, n + 2, nullptr, cc, afc_);
		if (!d0.ok) return false;
	}
	// The best alignment, not the first that passes: with the offset fitted
	// (below) a neighbouring word position can pass too, and a set-up
	// burst placed one word (10 ms) off made the real reference unit refuse our
	// B1 (RF bench 2026-10-15).
	double best_frac = 0, best_start = 0, best_bias = 0;
	double diag_frac = 0, diag_raw = 0; int diag_o = 99, diag_swap = -1, diag_seen = 0;   // fix 213c: the best alignment even when none passes (log)
	mw_partial_ = false;
	// Heard late: only pulses already heard are scored (pulse k on carrier c: centre d.c[c] + (k - 1) T,
	// reaching 2 T either side).
	const double heard_to = std::min(double(nn), double(to_x) + 2 * T_);
	auto heard = [&](const Demod &d, int car, int k) { return setup_heard_off() || d.c[car] + (k + 1) * T_ <= heard_to; };
	auto score = [&](const Demod &d) {
	for (int swap = 0; swap < 2; swap++)
		for (int o = -6; o <= 2; o++) {
			const auto &A = swap ? d.hi : d.lo, &B = swap ? d.lo : d.hi;
			const int ca = swap ? 1 : 0, cb = 1 - ca;
			int good = 0, seen = 0;
			// common offset first (the radios' frequency offset: from 15 Hz on
			// the set-up words no longer matched; see pattern_score), +-4/16
			// around the tracked offset
			std::complex<double> acc = 0;
			for (int k = 1; k <= n; k++) {
				const int i1 = 1 + o + k;
				if (i1 < 1 || i1 >= int(A.size()) || i1 >= int(B.size())) continue;
				if (!heard(d, ca, i1) || !heard(d, cb, i1)) continue;
				for (int c = 0; c < 2; c++) {
					const auto &Z = c ? B : A;
					const double want = c ? (w[k] & 15) : (w[k] >> 4);
					acc += std::polar(1.0, 2 * M_PI * (nib(Z[size_t(i1 - 1)], Z[size_t(i1)]) - afc_ - want) / 16.0);
				}
			}
			const double raw = 16.0 * std::arg(acc) / (2 * M_PI);
			// The clamped fit first; then the full one: at set-up nothing is
			// tracked yet (afc_ 0), and from 25 Hz (4/16) on the clamp threw
			// the offset away (emulator, 30 Hz: A1 never matched, our B1
			// went out on the guessed slot and was refused with R).
			const double cand[2] = {afc_ + (std::fabs(raw) > 4.0 ? 0.0 : raw), afc_ + raw};
			for (int h = 0; h < (std::fabs(raw) > 4.0 ? 2 : 1); h++) {
				const double bias = cand[h];
				good = 0; seen = 0;
				for (int k = 1; k <= n; k++) {
					const int i1 = 1 + o + k;
					if (i1 < 1 || i1 >= int(A.size()) || i1 >= int(B.size())) continue;
					if (!heard(d, ca, i1) || !heard(d, cb, i1)) continue;
					for (int c = 0; c < 2; c++) {
						const auto &Z = c ? B : A;
						const double v = nib(Z[size_t(i1 - 1)], Z[size_t(i1)]);
						const double want = c ? (w[k] & 15) : (w[k] >> 4);
						good += std::fabs(std::fmod(v - bias - want + 24.0, 16.0) - 8.0) < 2.0;
						seen++;
					}
				}
				if (seen > 0 && double(good) / seen > diag_frac) { diag_frac = double(good) / seen; diag_raw = raw; diag_o = o; diag_swap = swap; diag_seen = seen; }
				if (seen >= int(1.3 * n) && good >= int(0.85 * seen) && double(good) / seen > best_frac) {
					best_frac = double(good) / seen; best_start = d.start + o * T_; best_bias = bias;
				}
			}
		}
	};
	score(d0);
	// Fix 235: one carrier's first pulse found pulses away from the other's. The bench (f232-2,
	// the reference unit calling in P3, -18.6 Hz): "set-up words not matched: best 0.72, timing lo +70 hi
	// -359" at A1 and "best 0.71, lo +34 hi -301" at A2: the high carrier ~4-5 pulses early
	// against the low one (the two lie about half a pulse apart, either order), so half the
	// nibbles were read from the wrong pulses. A1 then decoded anyway (fix 213c), but A2 never
	// matched and the caller repeated it unanswered until its set-up timeout. Each carrier's
	// timing is tried taken from the other's, half a pulse later or earlier.
	// P3_SETUP_NO_REALIGN=1: off.
	const double stag = d0.c[1] - d0.c[0];
	if (best_frac <= 0 && std::fabs(stag) > 1.25 * T_ && !std::getenv("P3_SETUP_NO_REALIGN")) {
		for (int k = 0; k < 4 && best_frac <= 0; k++) {
			const double h = (k & 1 ? -0.5 : 0.5) * T_;
			double cc[2] = {d0.c[0], d0.c[1]};
			if (k < 2) cc[1] = d0.c[0] + h; else cc[0] = d0.c[1] - h;
			const Demod d2 = demod_cs(exp - samples(0.04), to_x, n + 2, nullptr, cc, afc_);
			if (!d2.ok) continue;
			score(d2);
			if (best_frac > 0) log("P3: set-up words matched with the " + std::string(k < 2 ? "high" : "low") + " carrier's timing taken from the other's (" +
			                       std::to_string(stag) + " samples apart; fix 235)");
		}
	}
	if (best_frac <= 0) {
		// Fix 257: a burst whose words heard so far all fit, too few of them heard yet (it began late:
		// a long audio path), for a later look at the whole of it (a2_quiet).
		mw_partial_ = diag_seen >= int(0.6 * n) && diag_frac >= 0.9;
		if (mw_partial_) {
			const Demod &dd = d0;
			mw_partial_at_ = int64_t(std::llround(dd.start + diag_o * T_));
		}
		if (debug_) log("set-up words not matched: best " + std::to_string(diag_frac) + " at word " + std::to_string(diag_o) + " swap " + std::to_string(diag_swap) +
		                ", residual " + std::to_string(diag_raw) + "/16, afc " + std::to_string(afc_hz()) + " Hz, timing lo " + std::to_string(d0.c[0] - double(exp)) + " hi " + std::to_string(d0.c[1] - double(exp)));
		return false;
	}
	start = best_start;
	// the peer's set-up words seed the offset everything after uses
	if (std::fabs(best_bias - afc_) > 2.0) afc_ = best_bias; else afc_update(best_bias, 16.0);
	return true;
}

}  // namespace fmodem

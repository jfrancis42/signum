// ic7300_cli.hpp: the command-line side of the IC-7300 set-up (src/radio/ic7300.hpp; docs/md/radio-ic7300.md
// "Zero-effort set-up"): --detect-ic7300, --configure-my-ic7300-and-overwrite-its-settings,
// --configure-my-ic7300-for-an-external-modem-and-overwrite-its-settings (the radio for a hardware modem on its ACC
// socket instead of Signum on USB), --restore-ic7300-settings FILE, --ic7300-serial N, and for scripts the equally long
// --i-understand-this-overwrites-my-ic7300-settings; --ic7300-power W [--measure] (RF POWER for a wanted P2/P3 peak
// power, src/radio/ic7300_power.hpp). One action per run; Signum exits afterwards.
//
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
#pragma once

#include "radio/ic7300.hpp"
#include "radio/ic7300_power.hpp"
#include "radio/ptt.hpp"
#include "start_config.hpp"

#include <alsa/asoundlib.h>
#include <dirent.h>
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <cmath>
#include <memory>
#include <thread>

#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace signum {

struct Ic7300Args {
	bool detect = false, configure = false, external = false, scripted = false, backup = false;
	std::string restore, serial, config;   // config: --config FILE ("" = the default; "none" = --no-config)
	double power = -1;                     // --ic7300-power W (< 0: not asked)
	bool measure = false;                  // --measure (with --ic7300-power: transmits)
	std::string tx_gain;                   // --tx-gain on the same command line ("" = the settings file's, else 0)
	bool any() const { return detect || backup || configure || external || !restore.empty() || power >= 0; }
};

// The IC-7300 options in argv; false and a message on a bad combination.
bool ic7300_args(int argc, char **argv, Ic7300Args &a, std::string &err);

// Programs other than us holding the device open (as fuser does; only our own user's are visible).
std::vector<std::string> ic7300_port_users(const std::string &dev);

bool ic7300_confirm(const Ic7300Args &a, const char *what);

// --measure's tone: a steady sine at 1500 Hz through the radio's codec (48 kHz mono S16), 10 ms ramps, played by a
// thread of its own until stop(). The card must be free (no Signum running on it).
class AlsaTone : public radio::ic7300::power::Tx {
public:
	explicit AlsaTone(std::string dev) : dev_(std::move(dev)) {}
	~AlsaTone() override { stop(); }
	bool start(double amplitude, std::string &why) override
	{
		stop();
		snd_pcm_t *h = nullptr;
		int e = snd_pcm_open(&h, dev_.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
		if (e < 0) { why = "cannot open " + dev_ + " (" + snd_strerror(e) + "): is Signum or another program using the radio's sound card?"; return false; }
		if ((e = snd_pcm_set_params(h, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 1, 48000, 1, 100000)) < 0) {
			snd_pcm_close(h);
			why = "cannot set " + dev_ + " to 48 kHz mono S16 (" + snd_strerror(e) + ")";
			return false;
		}
		h_ = h; run_ = true; amp_ = amplitude;
		th_ = std::thread([this] {
			std::vector<int16_t> b(480);
			double ph = 0, env = 0;
			const double dphi = 2.0 * M_PI * 1500.0 / 48000.0, step = 1.0 / 480.0;   // 10 ms ramp
			while (true) {
				const bool on = run_;
				for (auto &x : b) {
					env = on ? std::min(1.0, env + step) : std::max(0.0, env - step);
					x = int16_t(std::lround(amp_ * env * std::sin(ph)));
					ph += dphi; if (ph > 2 * M_PI) ph -= 2 * M_PI;
				}
				snd_pcm_sframes_t w = snd_pcm_writei(h_, b.data(), b.size());
				if (w < 0) w = snd_pcm_recover(h_, int(w), 1);
				if (w < 0) break;
				if (!on && env <= 0) break;
			}
		});
		return true;
	}
	void stop() override
	{
		run_ = false;
		if (th_.joinable()) th_.join();
		if (h_) { snd_pcm_drop(h_); snd_pcm_close(h_); h_ = nullptr; }
	}

private:
	std::string dev_;
	snd_pcm_t *h_ = nullptr;
	std::atomic<bool> run_{false};
	double amp_ = 0;
	std::thread th_;
};

inline std::atomic<bool> &ic7300_stop_flag() { static std::atomic<bool> f{false}; return f; }

// --ic7300-power W [--measure]: on one radio (the only one, or --ic7300-serial).
int ic7300_power_main(const Ic7300Args &a);

int ic7300_main(const Ic7300Args &a);

}  // namespace signum

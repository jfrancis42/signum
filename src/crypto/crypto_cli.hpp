// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// crypto_cli.hpp: the encryption feature's command-line options (docs/md/options.md "Encryption"), the
// one-shot key commands, and the check at start. Built only without SIGNUM_NO_CRYPTO.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>

#include "crypto_control.hpp"

namespace signum::crypto {

struct CryptoOptions {
	std::string encryption = "off", ack, key_dir, trust = "tofu", vtrust = "stored", sign = "off", compression = "on";
	double wait_s = 60;
	bool keygen = false, show_key = false;
	bool ack_none = false;       // --encryption-ack none: withdraw the stored acknowledgement
};

void crypto_usage(FILE *f);

// One argument; false: not ours. val() takes the option's value (exits on a missing one).
bool crypto_arg(const std::string &a, const std::function<const char *()> &val, CryptoOptions &o);

// After the arguments: loads the keys, applies the options. Returns -1 to go on, else the exit status
// (the one-shot commands, or a refusal: encryption on without the acknowledgement).
int crypto_setup(const CryptoOptions &o, CryptoControl &c, const std::string &mycall);

}  // namespace signum::crypto

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// legal.hpp: the texts shown whenever encryption is turned on (the command line, the console, the
// settings file, the WebSocket API for the GUI). Defined here once; every interface uses these.
#pragma once

namespace signum::crypto {

// The acknowledgement the user must give to turn encryption on (--encryption-ack, CRYPTO ON ...,
// "ack" in the WebSocket request): typed out, so that it is never given by accident.
inline constexpr char kAck[] = "NOT-AMATEUR";

inline constexpr char kLegalWarning[] =
    "WARNING: ENCRYPTION IS ILLEGAL ON THE AMATEUR RADIO BANDS. In the USA, 47 CFR 97.113(a)(4) forbids "
    "messages encoded for the purpose of obscuring their meaning, and most other countries have the same "
    "rule. Use Signum's encryption only where the law and your licence allow it (e.g. licensed "
    "commercial or government channels), never on amateur frequencies. You alone are responsible for "
    "using it lawfully.";

// Names the manufacturer on purpose (owner, 2026-11-16): a nominative statement, like README "Compatibility",
// so that nobody expects Signum to talk encrypted to that hardware.
inline constexpr char kNotCompatible[] =
    "Signum's encryption is NOT compatible with the AES encryption of SCS modems (the PTC series, the "
    "reference unit) or of any other modem: it works only between two Signum stations that both have it on.";

inline constexpr char kNoPlainFallback[] =
    "With encryption on, Signum never sends your data in clear: a link that cannot be encrypted is "
    "ended (with the reason in the log and the status), and broadcasts (unproto) are refused.";

}  // namespace signum::crypto

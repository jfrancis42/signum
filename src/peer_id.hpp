// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// peer_id.hpp: is the other station a Signum? The peer marker and the front panel's Signum / Other
// lamps (docs/md/gui.md "The front panel"; peer_caps.hpp).
//
// The marker is the peer capability record (peer_caps.hpp) with the product and version only
// ("FE1;pSignum;v0.5.1"), sent as an 'X' system-information sequence (0x1C 'X' body 0x20): the
// reference unit's SI layer skips it silently, shows nothing and acts on nothing. With encryption on,
// the encryption layer's own record (the same format, with its fields) is the marker, so the marker
// itself is sent only while encryption is off. Sent on P2/P3 links only, never on P1 (other P1
// equipment may print it). Owner's decision (2026-11-16): on by default (PEERmark, --peer-marker): a
// deliberate exception to "defaults as the reference unit".
//
// When it is sent: once per link, only while this station holds the sending direction anyway (at
// once as the caller, else at the first turn this station gets) or just before the first host text
// for the link, so that it never causes a changeover or a break-in, never delays a QRT asked for
// while receiving, and comes before this station's own text.
//
// The lamps: Signum (green) once a record from the other station has been read on this link (its
// marker, or its encryption record), with the switch on or off. Other (amber) is an inference: the
// marker is on here and the other station has sent kOtherAfter characters with no record before or
// among them (a Signum with its marker on sends it before its own text; only its P3 connect text,
// which is shorter, comes first). So Other means "not a Signum with its marker on": the reference
// unit, another P1/P2/P3 modem, an older Signum, or a Signum with its marker off. A record arriving
// later still turns the lamp to Signum. Both dark: standby, P1, the marker off without a record, or
// not known yet.
// No OS calls (the core may run on a small controller).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

#include "peer_caps.hpp"

namespace signum {

class PeerId {
public:
	static constexpr size_t kOtherAfter = 64;     // characters from the other station with no record: Other
	enum Kind { NONE, SIGNUM, OTHER };

	bool marker = true;                           // the switch (PEERmark, --peer-marker); read at each link's start

	// The marker's body: the capability record with the product and the version only.
	static std::string marker_body(const std::string &version)
	{
		Caps c;
		c.product = "Signum";
		c.version = version;
		return caps_body(c);
	}

	// A link is up (called until begun() is true). plevel: 1 P1, 2, 3; record: the encryption layer
	// sends its own record on this link (the marker is then not sent).
	void begin(int plevel, bool record)
	{
		*this = PeerId(marker);
		begun_ = true;
		plevel_ = plevel;
		marker_link_ = marker && plevel >= 2;
		due_ = marker_link_ && !record;
	}
	void end() { *this = PeerId(marker); }
	bool begun() const { return begun_; }
	// The marker is still to be sent on this link.
	bool due() const { return due_; }
	void sent() { due_ = false; }

	// From the other station, in the order received: n characters of text, an 'X' SI's body.
	// true: the lamp changed.
	bool text(size_t n)
	{
		if (!begun_ || kind_ == SIGNUM) return false;
		chars_ += n;
		if (kind_ == NONE && marker_link_ && chars_ >= kOtherAfter) { kind_ = OTHER; return true; }
		return false;
	}
	bool si(const std::string &body)
	{
		if (!begun_ || plevel_ < 2 || kind_ == SIGNUM) return false;
		const Caps c = parse_caps(body);
		if (!c.valid || c.product != "Signum") return false;
		kind_ = SIGNUM;
		version_ = c.version;
		return true;
	}

	Kind kind() const { return kind_; }
	const std::string &version() const { return version_; }   // the other Signum's version, from its record
	bool marker_link() const { return marker_link_; }         // the marker switch was on at this link's start (P2/P3)
	int plevel() const { return plevel_; }
	size_t chars() const { return chars_; }

	// The panel's "peer" object: {"lamp":"signum"|"other"|"off","version":..,"marker":0|1,"link":0|1|2|3}
	std::string json() const
	{
		std::string v;
		for (char c : version_) if (c != '"' && c != '\\' && uint8_t(c) >= 0x20) v += c;
		return std::string("{\"lamp\":\"") + (kind_ == SIGNUM ? "signum" : kind_ == OTHER ? "other" : "off") + "\",\"version\":\"" + v +
		       "\",\"marker\":" + (marker ? "1" : "0") + ",\"link\":" + std::to_string(begun_ ? plevel_ : 0) + "}";
	}
	// The console's words for this link (PEERmark's answer), "" with no link.
	std::string link_text() const
	{
		if (!begun_) return "";
		if (plevel_ < 2) return "THIS LINK: P1, NO MARKER";
		if (kind_ == SIGNUM) return "OTHER STATION: SIGNUM" + (version_.empty() ? std::string() : " " + version_);
		if (kind_ == OTHER) return "OTHER STATION: NOT A SIGNUM (NO MARKER IN ITS FIRST " + std::to_string(kOtherAfter) + " CHARACTERS)";
		return marker_link_ ? "OTHER STATION: NOT KNOWN YET" : "OTHER STATION: NOT KNOWN (MARKER OFF)";
	}

	PeerId() = default;
	explicit PeerId(bool m) : marker(m) {}

private:
	bool begun_ = false, marker_link_ = false, due_ = false;
	int plevel_ = 0;
	size_t chars_ = 0;
	Kind kind_ = NONE;
	std::string version_;
};

}  // namespace signum

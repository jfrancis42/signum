// tnc_core.hpp: the interface between signum's core (the modem, the reference unit
// command interpreter, audio, PTT, the pty) and its front ends (WebSocket
// server, web GUI, TUI, the command shell on a WebSocket).
//
// Threading: the core runs one loop (the audio loop; in FIFO mode the
// lockstep loop). EVERY call below is made on that loop's thread, from a
// front end's pump() (called by the loop about every 2.5 ms, never while a
// sample is being computed), and every callback is invoked on it too. A
// front end with threads of its own (ws_server.hpp's) hands data across
// with its own queues, as the WebSocket server does (post/commit/take).
// pump() must never block: the audio loop's timing is the modem's.
//
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>

namespace signum {

class Core {
public:
	virtual ~Core() = default;

	// ---- identity --------------------------------------------------------------
	virtual std::string version() const = 0;          // "Signum 0.1.0"
	virtual std::string mycall() const = 0;

	// ---- the front panel / status ------------------------------------------------
	// The panel state: one JSON object, the format src/util/panel_out.hpp
	// writes for the modem ({"src":"cpp","modem":"signum","text":..,"leds":{..},
	// "mode":{..},"detail":{..},"bar":..,"t":..}), so tui/signum-tui and the
	// GUI's render() read it unchanged. Empty until the first update.
	virtual const std::string &panel_json() const = 0;
	// Called with the new JSON whenever it changes (at most every 50 ms).
	virtual void on_panel(std::function<void(const std::string &json)> f) = 0;

	// ---- stream clients: the command interpreter, exactly as on the pty --------------
	// Each client gets its own interpreter state (terminal mode, cmd: prompt,
	// JHOST1/JHOST4 host mode, its own host-mode queues), as a second serial
	// port would. Received text and link events are delivered to every client
	// (each formats them for its own mode). Bytes in: the client's typing or
	// host frames; bytes out: everything the TNC would send on its serial port.
	// The pty is client 0. Returns the client id (> 0).
	virtual int attach(const std::string &name) = 0;
	virtual void detach(int client) = 0;
	virtual void input(int client, const uint8_t *p, size_t n) = 0;
	virtual std::string take_output(int client) = 0;   // bytes for the client (may hold 0xAA... in host mode)
	virtual bool host_mode(int client) const = 0;       // in JHOST1/JHOST4

	// ---- one-shot commands and the link --------------------------------------------
	// One terminal command line, answered with the text the terminal would show
	// (no prompt). JHOST is refused here (host mode belongs to a stream client).
	virtual std::string command(const std::string &line) = 0;
	virtual void connect(const std::string &call) = 0;  // as "C CALL"
	virtual void disconnect() = 0;                      // orderly (as "D")
	virtual void abort() = 0;                           // at once (as "DD")
	virtual void send(const std::string &text) = 0;     // text for the link (CR = new line)
	// The host's Ctrl-Y (host mode %O): break in while receiving, change over while sending; with
	// break_in_only (%I) only the break-in. false: nothing to do now (no link, or not built in).
	virtual bool changeover(bool /*break_in_only*/) { return false; }
	// The link now, one JSON object (src/web/README.md "link_status"): {"state":"standby|calling|connected",
	// "call","role":"caller|callee|"","plevel","level","unsent","unacked","held","rx_bytes","tx_bytes",
	// "link_s","reason"}; "" when not provided.
	virtual std::string link_json() const { return ""; }

	// ---- events -----------------------------------------------------------------
	virtual void on_rx(std::function<void(const std::string &text)> f) = 0;          // received link text
	virtual void on_link(std::function<void(bool up, const std::string &call)> f) = 0;
	virtual void on_log(std::function<void(const std::string &line)> f) = 0;          // the modem's log lines
	virtual void on_monitor(std::function<void(const std::string &who, const std::string &text)> f) = 0;   // listen mode
	// Audio as the modem hears it and sends it, 9600 Hz mono, one period at a time (waterfall).
	virtual void on_audio(std::function<void(const int16_t *rx, const int16_t *tx, size_t n)> f) = 0;

	// ---- settings (the GUI's Configure tab) -------------------------------------------
	// {"mycall":..,"mylevel":1-3,"tone_lo":..,"tone_hi":..,"mode":0-2,"listen":0/1,
	//  "maxerr":..,"txdelay":..,"pska":..,"fska":..,"cpu_adaptive":0/1,"p3_long_ceiling":0-6 (read only),
	//  "tune_s","tune_hz","tune_db","tx_gain","max_key","pduplex":0/1,"pdtimer":2-30,"cmsg","status","escchr","qrtchr","bkchr","chochr","urep",
	//  "alsa":..,"ptt":..,"pty":..}
	virtual std::string settings_json() const = 0;
	// Apply one key from settings_json (the station keys only): the interpreter answer, or "*** ERROR: ...".
	virtual std::string set(const std::string &key, const std::string &value) = 0;

	// ---- the settings file (src/start_config.hpp): what applies at the next start ------------
	// {"file":PATH or "","read":bool,"values":{option:value},"running":{option:value},"options":[every option the file takes]};
	// logins as "***".
	virtual std::string startup_json() const { return "{\"file\":\"\",\"read\":false,\"values\":{},\"running\":{}}"; }
	// Set one option in the file (value "" removes it): "" or why not.
	virtual std::string startup_set(const std::string &, const std::string &) { return "no settings file"; }

	// ---- the radio's own set-up (the GUI's IC-7300 set-up, src/radio/ic7300.hpp) ----------------------
	// No link, no link being set up, no unproto and no tuning tone: nothing that keys the transmitter.
	virtual bool quiet() const { return true; }
	// While held, the PTT never keys (a call answered meanwhile goes unanswered): the radio's settings are
	// being changed.
	virtual void hold_ptt(bool) {}

	// ---- encryption (src/crypto/, docs/md/encryption.md); absent unless built with encryption -------------
	// false: built without ./configure --enable-encryption (SIGNUM_NO_CRYPTO): no encryption code, options or
	// commands; the front ends hide every encryption control (the WebSocket hello's "crypto": false).
	virtual bool crypto_built() const { return false; }
	// The whole state as JSON (src/web/README.md "crypto"); "" when not built.
	virtual std::string crypto_status() const { return ""; }
	// One {"type":"crypto","op":...} request (its JSON text): the reply's fields ("ok", "text", ...).
	virtual std::string crypto_request(const std::string &) { return "\"ok\":false,\"text\":\"not built with encryption\""; }
	// Called (on the loop's thread) whenever the state changed.
	virtual void on_crypto(std::function<void()>) {}
};

// ---- what the front ends provide (src/web/frontend.cpp; src/frontend_none.cpp
// when there is none) ----------------------------------------------------------------
// The program's argument loop offers each argument to frontend_arg first: it
// consumes it (and its value: advance i) and returns true, or returns false.
bool frontend_arg(int &i, int argc, char **argv);
void frontend_usage(FILE *f);                         // its lines for --help
bool frontend_start(Core &core, std::string &err);    // after the core is up; false: exit with err
void frontend_pump(Core &core);                       // every loop period (~2.5 ms), on the loop's thread
void frontend_stop();                                 // at exit

}  // namespace signum
